#include <xen/config.h>
#include <xen/sched.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/domain_page.h>
#include <asm/flushtlb.h>
#include <asm/gic.h>

void dump_p2m_lookup(struct domain *d, paddr_t addr)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    lpae_t *first;

    printk("dom%d IPA 0x%"PRIpaddr"\n", d->domain_id, addr);

    printk("P2M @ %p mfn:0x%lx\n",
           p2m->first_level, page_to_mfn(p2m->first_level));

    first = __map_domain_page(p2m->first_level);
    dump_pt_walk(first, addr);
    unmap_domain_page(first);
}

void p2m_load_VTTBR(struct domain *d)
{
    if ( is_idle_domain(d) )
        return;
    BUG_ON(!d->arch.vttbr);
    WRITE_SYSREG64(d->arch.vttbr, VTTBR_EL2);
    isb(); /* Ensure update is visible */
}

/*
 * Lookup the MFN corresponding to a domain's PFN.
 *
 * There are no processor functions to do a stage 2 only lookup therefore we
 * do a a software walk.
 */
paddr_t p2m_lookup(struct domain *d, paddr_t paddr)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    lpae_t pte, *first = NULL, *second = NULL, *third = NULL;
    paddr_t maddr = INVALID_PADDR;

    spin_lock(&p2m->lock);

    first = __map_domain_page(p2m->first_level);

    pte = first[first_table_offset(paddr)];
    if ( !pte.p2m.valid || !pte.p2m.table )
        goto done;

    second = map_domain_page(pte.p2m.base);
    pte = second[second_table_offset(paddr)];
    if ( !pte.p2m.valid || !pte.p2m.table )
        goto done;

    third = map_domain_page(pte.p2m.base);
    pte = third[third_table_offset(paddr)];

    /* This bit must be one in the level 3 entry */
    if ( !pte.p2m.table )
        pte.bits = 0;

done:
    if ( pte.p2m.valid )
        maddr = (pte.bits & PADDR_MASK & PAGE_MASK) | (paddr & ~PAGE_MASK);

    if (third) unmap_domain_page(third);
    if (second) unmap_domain_page(second);
    if (first) unmap_domain_page(first);

    spin_unlock(&p2m->lock);

    return maddr;
}

int guest_physmap_mark_populate_on_demand(struct domain *d,
                                          unsigned long gfn,
                                          unsigned int order)
{
    return -ENOSYS;
}

int p2m_pod_decrease_reservation(struct domain *d,
                                 xen_pfn_t gpfn,
                                 unsigned int order)
{
    return -ENOSYS;
}

/* Allocate a new page table page and hook it in via the given entry */
static int p2m_create_table(struct domain *d,
                            lpae_t *entry)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    struct page_info *page;
    void *p;
    lpae_t pte;

    BUG_ON(entry->p2m.valid);

    page = alloc_domheap_page(NULL, 0);
    if ( page == NULL )
        return -ENOMEM;

    page_list_add(page, &p2m->pages);

    p = __map_domain_page(page);
    clear_page(p);
    unmap_domain_page(p);

    pte = mfn_to_p2m_entry(page_to_mfn(page), MATTR_MEM);

    write_pte(entry, pte);

    return 0;
}

enum p2m_operation {
    INSERT,
    ALLOCATE,
    REMOVE
};

static int create_p2m_entries(struct domain *d,
                     enum p2m_operation op,
                     paddr_t start_gpaddr,
                     paddr_t end_gpaddr,
                     paddr_t maddr,
                     int mattr)
{
    int rc, flush;
    struct p2m_domain *p2m = &d->arch.p2m;
    lpae_t *first = NULL, *second = NULL, *third = NULL;
    paddr_t addr;
    unsigned long cur_first_offset = ~0, cur_second_offset = ~0;

    spin_lock(&p2m->lock);

    /* XXX Don't actually handle 40 bit guest physical addresses */
    BUG_ON(start_gpaddr & 0x8000000000ULL);
    BUG_ON(end_gpaddr   & 0x8000000000ULL);

    first = __map_domain_page(p2m->first_level);

    for(addr = start_gpaddr; addr < end_gpaddr; addr += PAGE_SIZE)
    {
        if ( !first[first_table_offset(addr)].p2m.valid )
        {
            rc = p2m_create_table(d, &first[first_table_offset(addr)]);
            if ( rc < 0 ) {
                printk("p2m_populate_ram: L1 failed\n");
                goto out;
            }
        }

        BUG_ON(!first[first_table_offset(addr)].p2m.valid);

        if ( cur_first_offset != first_table_offset(addr) )
        {
            if (second) unmap_domain_page(second);
            second = map_domain_page(first[first_table_offset(addr)].p2m.base);
            cur_first_offset = first_table_offset(addr);
        }
        /* else: second already valid */

        if ( !second[second_table_offset(addr)].p2m.valid )
        {
            rc = p2m_create_table(d, &second[second_table_offset(addr)]);
            if ( rc < 0 ) {
                printk("p2m_populate_ram: L2 failed\n");
                goto out;
            }
        }

        BUG_ON(!second[second_table_offset(addr)].p2m.valid);

        if ( cur_second_offset != second_table_offset(addr) )
        {
            /* map third level */
            if (third) unmap_domain_page(third);
            third = map_domain_page(second[second_table_offset(addr)].p2m.base);
            cur_second_offset = second_table_offset(addr);
        }

        flush = third[third_table_offset(addr)].p2m.valid;

        /* Allocate a new RAM page and attach */
        switch (op) {
            case ALLOCATE:
                {
                    struct page_info *page;
                    lpae_t pte;

                    rc = -ENOMEM;
                    page = alloc_domheap_page(d, 0);
                    if ( page == NULL ) {
                        printk("p2m_populate_ram: failed to allocate page\n");
                        goto out;
                    }

                    pte = mfn_to_p2m_entry(page_to_mfn(page), mattr);

                    write_pte(&third[third_table_offset(addr)], pte);
                }
                break;
            case INSERT:
                {
                    lpae_t pte = mfn_to_p2m_entry(maddr >> PAGE_SHIFT, mattr);
                    write_pte(&third[third_table_offset(addr)], pte);
                    maddr += PAGE_SIZE;
                }
                break;
            case REMOVE:
                {
                    lpae_t pte;
                    memset(&pte, 0x00, sizeof(pte));
                    write_pte(&third[third_table_offset(addr)], pte);
                    maddr += PAGE_SIZE;
                }
                break;
        }

        if ( flush )
            flush_tlb_all_local();
    }

    rc = 0;

out:
    if (third) unmap_domain_page(third);
    if (second) unmap_domain_page(second);
    if (first) unmap_domain_page(first);

    spin_unlock(&p2m->lock);

    return rc;
}

int p2m_protect_ram(struct domain *d, paddr_t start, paddr_t end, int prot)
{
    int rc, flush;
    struct p2m_domain *p2m = &d->arch.p2m;
    lpae_t *first = NULL, *second = NULL, *third = NULL, pte;
    paddr_t addr;
    unsigned long cur_first_offset = ~0, cur_second_offset = ~0;

    spin_lock(&p2m->lock);

    /* XXX Don't actually handle 40 bit guest physical addresses */
    BUG_ON(start & 0x8000000000ULL);
    BUG_ON(end   & 0x8000000000ULL);

    first = __map_domain_page(p2m->first_level);

    for(addr = start; addr < end; addr += PAGE_SIZE)
    {
        if ( !first[first_table_offset(addr)].p2m.valid )
        {
            rc = -EINVAL;
            goto out;
        }

        BUG_ON(!first[first_table_offset(addr)].p2m.valid);

        if ( cur_first_offset != first_table_offset(addr) )
        {
            if (second) unmap_domain_page(second);
            second = map_domain_page(first[first_table_offset(addr)].p2m.base);
            cur_first_offset = first_table_offset(addr);
        }

        if ( !second[second_table_offset(addr)].p2m.valid )
        {
            rc = -EINVAL;
            goto out;
        }

        BUG_ON(!second[second_table_offset(addr)].p2m.valid);

        if ( cur_second_offset != second_table_offset(addr) )
        {
            if (third) unmap_domain_page(third);
            third = map_domain_page(second[second_table_offset(addr)].p2m.base);
            cur_second_offset = second_table_offset(addr);
        }

        if ( !third[third_table_offset(addr)].p2m.valid )
        {
            rc = -EINVAL;
            goto out;
        }

        pte = third[third_table_offset(addr)];

        if ( prot & P2M_PROT_READ )
            pte.p2m.read = 1;
        else
            pte.p2m.read = 0;

        if ( prot & P2M_PROT_WRITE )
            pte.p2m.write = 1;
        else
            pte.p2m.write = 0;

        if ( prot & P2M_PROT_EXEC )
            pte.p2m.xn = 0;
        else
            pte.p2m.xn = 1;

        flush = (pte.bits != third[third_table_offset(addr)].bits);

        write_pte(&third[third_table_offset(addr)], pte);

        if ( flush )
            flush_tlb_all_local();
    }

    rc = 0;

out:
    if (third) unmap_domain_page(third);
    if (second) unmap_domain_page(second);
    if (first) unmap_domain_page(first);

    spin_unlock(&p2m->lock);

    return rc;
}

int p2m_populate_ram(struct domain *d,
                     paddr_t start,
                     paddr_t end)
{
    return create_p2m_entries(d, ALLOCATE, start, end, 0, MATTR_MEM);
}

int map_mmio_regions(struct domain *d,
                     paddr_t start_gaddr,
                     paddr_t end_gaddr,
                     paddr_t maddr)
{
    return create_p2m_entries(d, INSERT, start_gaddr, end_gaddr, maddr, MATTR_DEV);
}

int guest_physmap_add_page(struct domain *d,
                           unsigned long gpfn,
                           unsigned long mfn,
                           unsigned int page_order)
{
    return create_p2m_entries(d, INSERT, gpfn << PAGE_SHIFT,
                              (gpfn + (1<<page_order)) << PAGE_SHIFT,
                              mfn << PAGE_SHIFT, MATTR_MEM);
}

void guest_physmap_remove_page(struct domain *d,
                               unsigned long gpfn,
                               unsigned long mfn, unsigned int page_order)
{
    create_p2m_entries(d, REMOVE, gpfn << PAGE_SHIFT,
                       (gpfn + (1<<page_order)) << PAGE_SHIFT,
                       mfn << PAGE_SHIFT, MATTR_MEM);
}

int p2m_alloc_table(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    struct page_info *page;
    void *p;

    /* First level P2M is 2 consecutive pages */
    page = alloc_domheap_pages(NULL, 1, 0);
    if ( page == NULL )
        return -ENOMEM;

    spin_lock(&p2m->lock);

    page_list_add(page, &p2m->pages);

    /* Clear both first level pages */
    p = __map_domain_page(page);
    clear_page(p);
    unmap_domain_page(p);

    p = __map_domain_page(page + 1);
    clear_page(p);
    unmap_domain_page(p);

    p2m->first_level = page;

    d->arch.vttbr = page_to_maddr(p2m->first_level)
        | ((uint64_t)p2m->vmid&0xff)<<48;

    spin_unlock(&p2m->lock);

    return 0;
}

void p2m_teardown(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    struct page_info *pg;

    spin_lock(&p2m->lock);

    while ( (pg = page_list_remove_head(&p2m->pages)) )
        free_domheap_page(pg);

    p2m->first_level = NULL;

    spin_unlock(&p2m->lock);
}

int p2m_init(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;

    spin_lock_init(&p2m->lock);
    INIT_PAGE_LIST_HEAD(&p2m->pages);

    /* XXX allocate properly */
    /* Zero is reserved */
    p2m->vmid = d->domain_id + 1;

    d->arch.vttbr = 0;

    p2m->first_level = NULL;

    return 0;
}

unsigned long gmfn_to_mfn(struct domain *d, unsigned long gpfn)
{
    paddr_t p = p2m_lookup(d, gpfn << PAGE_SHIFT);
    return p >> PAGE_SHIFT;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
