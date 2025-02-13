/*
 *  mm/pgtable-generic.c
 *
 *  Generic pgtable methods declared in asm-generic/pgtable.h
 *
 *  Copyright (C) 2010  Linus Torvalds
 */

#include <linux/pagemap.h>
#include <asm/tlb.h>
#include <asm-generic/pgtable.h>

/*
 * If a p?d_bad entry is found while walking page tables, report
 * the error, before resetting entry to p?d_none.  Usually (but
 * very seldom) called out from the p?d_none_or_clear_bad macros.
 */

void pgd_clear_bad(pgd_t *pgd)
{
	pgd_ERROR(*pgd);
	pgd_clear(pgd);
}

void pud_clear_bad(pud_t *pud)
{
	pud_ERROR(*pud);
	pud_clear(pud);
}

void pmd_clear_bad(pmd_t *pmd)
{
	pmd_ERROR(*pmd);
	pmd_clear(pmd);
}

#ifndef __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
/*
 * Only sets the access flags (dirty, accessed), as well as write 
 * permission. Furthermore, we know it always gets set to a "more
 * permissive" setting, which allows most architectures to optimize
 * this. We return whether the PTE actually changed, which in turn
 * instructs the caller to do things like update__mmu_cache.  This
 * used to be done in the caller, but sparc needs minor faults to
 * force that call on sun4c so we changed this macro slightly
 */
int ptep_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pte_t *ptep,
			  pte_t entry, int dirty)
{
	int changed = !pte_same(*ptep, entry);
	if (changed) {
		set_pte_at(vma->vm_mm, address, ptep, entry);
		flush_tlb_fix_spurious_fault(vma, address);
	}
	return changed;
}
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_YOUNG_FLUSH
int ptep_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep)
{
	int young;
	young = ptep_test_and_clear_young(vma, address, ptep);
	if (young)
		flush_tlb_page(vma, address);
	return young;
}
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_FLUSH
pte_t ptep_clear_flush(struct vm_area_struct *vma, unsigned long address,
		       pte_t *ptep)
{
	struct mm_struct *mm = (vma)->vm_mm;
	pte_t pte;
	pte = ptep_get_and_clear(mm, address, ptep);
	if (pte_accessible(mm, pte))
		flush_tlb_page(vma, address);
	return pte;
}
#endif

#if 0
#ifdef CONFIG_TRANSPARENT_HUGEPAGE

#ifndef __HAVE_ARCH_FLUSH_PMD_TLB_RANGE

/*
 * ARCHes with special requirements for evicting THP backing TLB entries can
 * implement this. Otherwise also, it can help optimize normal TLB flush in
 * THP regime. stock flush_tlb_range() typically has optimization to nuke the
 * entire TLB TLB if flush span is greater than a threshhold, which will
 * likely be true for a single huge page. Thus a single thp flush will
 * invalidate the entire TLB which is not desitable.
 * e.g. see arch/arc: flush_pmd_tlb_range
 */
#define flush_pmd_tlb_range(vma, addr, end)	flush_tlb_range(vma, addr, end)
#endif

#ifndef __HAVE_ARCH_PMDP_SET_ACCESS_FLAGS
int pmdp_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pmd_t *pmdp,
			  pmd_t entry, int dirty)
{
	int changed = !pmd_same(*pmdp, entry);
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	if (changed) {
		set_pmd_at(vma->vm_mm, address, pmdp, entry);
		flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	}
	return changed;
}
#endif

#ifndef __HAVE_ARCH_PMDP_CLEAR_YOUNG_FLUSH
int pmdp_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pmd_t *pmdp)
{
	int young;
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	young = pmdp_test_and_clear_young(vma, address, pmdp);
	if (young)
		flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	return young;
}
#endif

#ifndef __HAVE_ARCH_PMDP_HUGE_CLEAR_FLUSH
pmd_t pmdp_huge_clear_flush(struct vm_area_struct *vma, unsigned long address,
			    pmd_t *pmdp)
{
	pmd_t pmd;
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	VM_BUG_ON(!pmd_trans_huge(*pmdp));
	pmd = pmdp_huge_get_and_clear(vma->vm_mm, address, pmdp);
	flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	return pmd;
}
#endif

#ifndef __HAVE_ARCH_PMDP_SPLITTING_FLUSH
void pmdp_splitting_flush(struct vm_area_struct *vma, unsigned long address,
			  pmd_t *pmdp)
{
	pmd_t pmd = pmd_mksplitting(*pmdp);
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	set_pmd_at(vma->vm_mm, address, pmdp, pmd);
	/* tlb flush only to serialize against gup-fast */
	flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
}
#endif

#ifndef __HAVE_ARCH_PGTABLE_DEPOSIT
void pgtable_trans_huge_deposit(struct mm_struct *mm, pmd_t *pmdp,
				pgtable_t pgtable)
{
	assert_spin_locked(pmd_lockptr(mm, pmdp));

	/* FIFO */
	if (!pmd_huge_pte(mm, pmdp))
		INIT_LIST_HEAD(&pgtable->lru);
	else
		list_add(&pgtable->lru, &pmd_huge_pte(mm, pmdp)->lru);
	pmd_huge_pte(mm, pmdp) = pgtable;
}
#endif

#ifndef __HAVE_ARCH_PGTABLE_WITHDRAW
/* no "address" argument so destroys page coloring of some arch */
pgtable_t pgtable_trans_huge_withdraw(struct mm_struct *mm, pmd_t *pmdp)
{
	pgtable_t pgtable;

	assert_spin_locked(pmd_lockptr(mm, pmdp));

	/* FIFO */
	pgtable = pmd_huge_pte(mm, pmdp);
	if (list_empty(&pgtable->lru))
		pmd_huge_pte(mm, pmdp) = NULL;
	else {
		pmd_huge_pte(mm, pmdp) = list_entry(pgtable->lru.next,
					      struct page, lru);
		list_del(&pgtable->lru);
	}
	return pgtable;
}
#endif

#ifndef __HAVE_ARCH_PMDP_INVALIDATE
void pmdp_invalidate(struct vm_area_struct *vma, unsigned long address,
		     pmd_t *pmdp)
{
	pmd_t entry = *pmdp;
	set_pmd_at(vma->vm_mm, address, pmdp, pmd_mknotpresent(entry));
	flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
}
#endif

#ifndef pmdp_collapse_flush
pmd_t pmdp_collapse_flush(struct vm_area_struct *vma, unsigned long address,
			  pmd_t *pmdp)
{
	/*
	 * pmd and hugepage pte format are same. So we could
	 * use the same function.
	 */
	pmd_t pmd;

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	VM_BUG_ON(pmd_trans_huge(*pmdp));
	pmd = pmdp_huge_get_and_clear(vma->vm_mm, address, pmdp);

	/* collapse entails shooting down ptes not pmd */
	flush_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	return pmd;
}
#endif
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif
