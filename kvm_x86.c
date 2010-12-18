
#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/modctl.h>
#include <sys/open.h>
#include <sys/kmem.h>
#include <sys/poll.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/atomic.h>
#include <sys/spl.h>
#include <sys/thread.h>
#include <sys/cpuvar.h>
#include <vm/hat_i86.h>
#include <sys/segments.h>
#include <sys/mman.h>

#include "msr-index.h"
#include "msr.h"
#include "vmx.h"
#include "processor-flags.h"
#include "apicdef.h"
#include "kvm_types.h"
#include "kvm_host.h"
#include "kvm_x86host.h"
#include "iodev.h"

#define PER_CPU_ATTRIBUTES
#define PER_CPU_DEF_ATTRIBUTES
#define PER_CPU_BASE_SECTION ".data"
#include "percpu-defs.h"
#include "coalesced_mmio.h"
#include "kvm.h"
#include "ioapic.h"
#include "irq.h"

extern struct vmcs **vmxarea;

static int vcpuid;
extern uint64_t native_read_msr_safe(unsigned int msr,
				     int *err);
extern int native_write_msr_safe(unsigned int msr,
				 unsigned low, unsigned high);

unsigned long segment_base(uint16_t selector)
{
	struct descriptor_table gdt;
	struct desc_struct *d;
	unsigned long table_base;
	unsigned long v;

	if (selector == 0)
		return 0;

	kvm_get_gdt(&gdt);
	table_base = gdt.base;

	if (selector & 4) {           /* from ldt */
		uint16_t ldt_selector = kvm_read_ldt();

		table_base = segment_base(ldt_selector);
	}
	d = (struct desc_struct *)(table_base + (selector & ~7));
	v = get_desc_base(d);

#ifdef CONFIG_X86_64
	if (d->c.b.s == 0 && (d->c.b.type == 2 || d->c.b.type == 9 || d->c.b.type == 11))
		v |= ((unsigned long)((struct ldttss_desc64 *)d)->base3) << 32;
#endif

	return v;
}


struct  kvm *
kvm_arch_create_vm(void)
{
	struct kvm *kvm = kmem_zalloc(sizeof(struct kvm), KM_SLEEP);

	if (!kvm)
		return NULL;

	kvm->arch.aliases = kmem_zalloc(sizeof(struct kvm_mem_aliases), KM_SLEEP);
	if (!kvm->arch.aliases) {
		kmem_free(kvm, sizeof(struct kvm));
		return NULL;
	}

	list_create(&kvm->arch.active_mmu_pages, sizeof(struct kvm_mmu_page),
		    offsetof(struct kvm_mmu_page, link));

	list_create(&kvm->arch.assigned_dev_head, sizeof(struct kvm_assigned_dev_kernel),
		    offsetof(struct kvm_assigned_dev_kernel, list));

	/* Reserve bit 0 of irq_sources_bitmap for userspace irq source */
	kvm->arch.irq_sources_bitmap |= KVM_USERSPACE_IRQ_SOURCE_ID;

	/* XXX - original is rdtscll() */
	kvm->arch.vm_init_tsc = (uint64_t)gethrtime(); 

	return kvm;
}

inline gpa_t gfn_to_gpa(gfn_t gfn)
{
	return (gpa_t)gfn << PAGESHIFT;
}

#ifdef IOMMU

paddr_t
iommu_iova_to_phys(struct iommu_domain *domain,
			       unsigned long iova)
{
	return iommu_ops->iova_to_phys(domain, iova);
}

static void kvm_iommu_put_pages(struct kvm *kvm,
				gfn_t base_gfn, unsigned long npages)
{
	gfn_t gfn = base_gfn;
	pfn_t pfn;
	struct iommu_domain *domain = kvm->arch.iommu_domain;
	unsigned long i;
	uint64_t phys;

	/* check if iommu exists and in use */
	if (!domain)
		return;

	for (i = 0; i < npages; i++) {
		phys = iommu_iova_to_phys(domain, gfn_to_gpa(gfn));
		pfn = phys >> PAGESHIFT;
		kvm_release_pfn_clean(pfn);
		gfn++;
	}

	iommu_unmap_range(domain, gfn_to_gpa(base_gfn), PAGESIZE * npages);
}

static int
kvm_iommu_unmap_memslots(struct kvm *kvm)
{
	int i;
	struct kvm_memslots *slots;

	slots = kvm->memslots;

	for (i = 0; i < slots->nmemslots; i++) {
		kvm_iommu_put_pages(kvm, slots->memslots[i].base_gfn,
				    slots->memslots[i].npages);
	}

	return 0;
}

int
kvm_iommu_unmap_guest(struct kvm *kvm)
{
	struct iommu_domain *domain = kvm->arch.iommu_domain;

	/* check if iommu exists and in use */
	if (!domain)
		return 0;

	kvm_iommu_unmap_memslots(kvm);
	iommu_domain_free(domain);
	return 0;
}
#endif /*IOMMU*/

void
kvm_arch_destroy_vm(struct kvm *kvm)
{
	if (!kvm)
		return;  /* nothing to do here */

#ifdef IOMMU
	kvm_iommu_unmap_guest(kvm);
#endif /*IOMMU*/
#ifdef PIT /* i8254 programmable interrupt timer support */
	kvm_free_pit(kvm);
#endif /*PIT*/
#ifdef VPIC
	kmem_free(kvm->arch.vpic);
	kfree(kvm->arch.vioapic);
#endif /*VPIC*/
#ifdef XXX
	kvm_free_vcpus(kvm);
	kvm_free_physmem(kvm);
#endif
#ifdef APIC
	if (kvm->arch.apic_access_page)
		put_page(kvm->arch.apic_access_page);
	if (kvm->arch.ept_identity_pagetable)
		put_page(kvm->arch.ept_identity_pagetable);
#endif /*APIC*/
#if defined(CONFIG_MMU_NOTIFIER) && defined(KVM_ARCH_WANT_MMU_NOTIFIER)
	cleanup_srcu_struct(&kvm->srcu);
#endif /*CONFIG_MMU_NOTIFIER && KVM_ARCH_WANT_MMU_NOTIFIER*/
	kmem_free(kvm->arch.aliases, sizeof (struct kvm_mem_aliases));
	kmem_free(kvm, sizeof(struct kvm));
}

extern int getcr4(void);
extern void setcr4(ulong_t val);
extern int getcr0(void);
extern ulong_t getcr3(void);
extern pfn_t hat_getpfnum(struct hat *hat, caddr_t);

#define X86_CR4_VMXE	0x00002000 /* enable VMX virtualization */
#define MSR_IA32_FEATURE_CONTROL        0x0000003a

#define FEATURE_CONTROL_LOCKED		(1<<0)
#define FEATURE_CONTROL_VMXON_ENABLED	(1<<2)

#define ASM_VMX_VMXON_RAX         ".byte 0xf3, 0x0f, 0xc7, 0x30"

extern uint64_t shadow_trap_nonpresent_pte;
extern uint64_t shadow_notrap_nonpresent_pte;
extern uint64_t shadow_base_present_pte;
extern uint64_t shadow_nx_mask;
extern uint64_t shadow_x_mask;	/* mutual exclusive with nx_mask */
extern uint64_t shadow_user_mask;
extern uint64_t shadow_accessed_mask;
extern uint64_t shadow_dirty_mask;

extern pfn_t hat_getpfnum(hat_t *hat, caddr_t addr);
struct vmcs_config vmcs_config;

int
vmx_hardware_enable(void *garbage)
{
	int cpu = curthread->t_cpu->cpu_seqid;
	pfn_t pfn;
	uint64_t old;
#ifdef XXX
	uint64_t phys_addr = kvtop(per_cpu(vmxarea, cpu));
#else
	uint64_t phys_addr;
	volatile int x;  /* XXX - dtrace return probe missing */
	pfn = hat_getpfnum(kas.a_hat, (caddr_t)vmxarea[cpu]);
	phys_addr = ((uint64_t)pfn << PAGESHIFT)|((uint64_t)vmxarea[cpu] & PAGEOFFSET);
#endif

	((struct vmcs *)(vmxarea[cpu]))->revision_id = vmcs_config.revision_id;

	if (getcr4() & X86_CR4_VMXE)
		return DDI_FAILURE;

#ifdef XXX
	INIT_LIST_HEAD(&per_cpu(vcpus_on_cpu, cpu));
#endif
	rdmsrl(MSR_IA32_FEATURE_CONTROL, old);
	if ((old & (FEATURE_CONTROL_LOCKED |
		    FEATURE_CONTROL_VMXON_ENABLED))
	    != (FEATURE_CONTROL_LOCKED |
		FEATURE_CONTROL_VMXON_ENABLED))
		/* enable and lock */
		wrmsrl(MSR_IA32_FEATURE_CONTROL, old |
		       FEATURE_CONTROL_LOCKED |
		       FEATURE_CONTROL_VMXON_ENABLED);
	setcr4(getcr4() | X86_CR4_VMXE); /* FIXME: not cpu hotplug safe */
	asm volatile (ASM_VMX_VMXON_RAX
		      : : "a"(&phys_addr), "m"(phys_addr)
		      : "memory", "cc");

#ifdef XXX
	ept_sync_global();
#endif /*XXX*/

	x = 10; /*XXX*/
	return 0;
}

extern struct vcpu_vmx *to_vmx(struct kvm_vcpu *vcpu);
extern void vmcs_writel(unsigned long field, unsigned long value);
extern unsigned long vmcs_readl(unsigned long field);

unsigned long vmx_get_rflags(struct kvm_vcpu *vcpu)
{
	unsigned long rflags, save_rflags;

	rflags = vmcs_readl(GUEST_RFLAGS);
	if (to_vmx(vcpu)->rmode.vm86_active) {
		rflags &= RMODE_GUEST_OWNED_EFLAGS_BITS;
		save_rflags = to_vmx(vcpu)->rmode.save_rflags;
		rflags |= save_rflags & ~RMODE_GUEST_OWNED_EFLAGS_BITS;
	}
	return rflags;
}
void vmx_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags)
{
	if (to_vmx(vcpu)->rmode.vm86_active) {
		to_vmx(vcpu)->rmode.save_rflags = rflags;
		rflags |= X86_EFLAGS_IOPL | X86_EFLAGS_VM;
	}
	vmcs_writel(GUEST_RFLAGS, rflags);
}

int kvm_arch_hardware_enable(void *garbage)
{
#ifdef LATER
	/*
	 * Since this may be called from a hotplug notifcation,
	 * we can't get the CPU frequency directly.
	 */
	if (!boot_cpu_has(X86_FEATURE_CONSTANT_TSC)) {
		int cpu = raw_smp_processor_id();
		per_cpu(cpu_tsc_khz, cpu) = 0;
	}

	kvm_shared_msr_cpu_online();
#endif

	return vmx_hardware_enable(garbage);
}

void kvm_arch_hardware_disable(void *garbage)
{
#ifdef XXX
	hardware_disable(garbage);
#endif /*XXX*/
#if defined(CONFIG_MMU_NOTIFIER) && defined(KVM_ARCH_WANT_MMU_NOTIFIER)
	drop_user_return_notifiers(garbage);
#endif /*CONFIG_MMU_NOTIFIER && KVM_ARCH_WANT_MMU_NOTIFIER*/
}

int
kvm_dev_ioctl_check_extension(long ext, int *rval_p)
{
	int r;

	switch (ext) {
	case KVM_CAP_IRQCHIP:
	case KVM_CAP_HLT:
	case KVM_CAP_MMU_SHADOW_CACHE_CONTROL:
	case KVM_CAP_SET_TSS_ADDR:
	case KVM_CAP_EXT_CPUID:
	case KVM_CAP_CLOCKSOURCE:
	case KVM_CAP_PIT:
	case KVM_CAP_NOP_IO_DELAY:
	case KVM_CAP_MP_STATE:
	case KVM_CAP_SYNC_MMU:
	case KVM_CAP_REINJECT_CONTROL:
	case KVM_CAP_IRQ_INJECT_STATUS:
	case KVM_CAP_ASSIGN_DEV_IRQ:
	case KVM_CAP_IRQFD:
	case KVM_CAP_IOEVENTFD:
	case KVM_CAP_PIT2:
	case KVM_CAP_PIT_STATE2:
	case KVM_CAP_SET_IDENTITY_MAP_ADDR:
	case KVM_CAP_XEN_HVM:
	case KVM_CAP_ADJUST_CLOCK:
	case KVM_CAP_VCPU_EVENTS:
	case KVM_CAP_HYPERV:
	case KVM_CAP_HYPERV_VAPIC:
	case KVM_CAP_HYPERV_SPIN:
	case KVM_CAP_PCI_SEGMENT:
	case KVM_CAP_X86_ROBUST_SINGLESTEP:
		*rval_p = 1;
		r = DDI_SUCCESS;
		break;
	case KVM_CAP_COALESCED_MMIO:
#ifdef KVM_COALESCED_MMIO_PAGE_OFFSET
		*rval_p = KVM_COALESCED_MMIO_PAGE_OFFSET;
		r = DDI_SUCCESS;
		break;
#else
		r = EINVAL;
		break;
#endif/*KVM_COALESCED_MMIO_PAGE_OFFSET*/
	case KVM_CAP_VAPIC:
#ifdef XXX
		*rval_p = !kvm_x86_ops->cpu_has_accelerated_tpr();
		r = DDI_SUCCESS;
#else
		r = EINVAL;
#endif /*XXX*/
		break;
	case KVM_CAP_NR_VCPUS:
		*rval_p = KVM_MAX_VCPUS;
		r = DDI_SUCCESS;
		break;
	case KVM_CAP_NR_MEMSLOTS:
		*rval_p = KVM_MEMORY_SLOTS;
		r = DDI_SUCCESS;
		break;
	case KVM_CAP_PV_MMU:	/* obsolete */
		r = EINVAL;
		break;
	case KVM_CAP_IOMMU:
#ifdef XXX
		*rval_p = iommu_found();
		r = DDI_SUCCESS;
#else
		r = EINVAL;
#endif /*XXX*/
		break;
	case KVM_CAP_MCE:
		*rval_p = KVM_MAX_MCE_BANKS;
		r = DDI_SUCCESS;
		break;
	default:
		r = EINVAL;
		break;
	}
	return r;
}

int irqchip_in_kernel(struct kvm *kvm)
{
	int ret;

	ret = (pic_irqchip(kvm) != NULL);
#ifdef XXX
	smp_rmb();
#endif
	return ret;
}

static int alloc_mmu_pages(struct kvm_vcpu *vcpu)
{
	caddr_t page;
	int i;

	ASSERT(vcpu);

	/*
	 * When emulating 32-bit mode, cr3 is only 32 bits even on x86_64.
	 * Therefore we need to allocate shadow page tables in the first
	 * 4GB of memory, which happens to fit the DMA32 zone.
	 * XXX - for right now, ignore DMA32.  need to use ddi_dma_mem_alloc
	 * to address this issue...
	 * XXX - also, don't need to allocate a full page, we'll look
	 * at htable_t later on solaris.
	 */
	page = kmem_zalloc(PAGESIZE, KM_SLEEP);
	if (!page)
		return -ENOMEM;

	vcpu->arch.mmu.pae_root = (uint64_t *)page;

	/* XXX - why only 4?  must be physical address extension */
	/* which is used for 32-bit guest virtual with 36-bit physical, */
	/* and 32-bit on 64-bit hardware */
	/* unclear what happens for 64bit guest on 64 bit hw */

	for (i = 0; i < 4; ++i)
		vcpu->arch.mmu.pae_root[i] = INVALID_PAGE;

	return 0;
}

int kvm_mmu_create(struct kvm_vcpu *vcpu)
{
	int i;

	ASSERT(vcpu);
	ASSERT(!VALID_PAGE(vcpu->arch.mmu.root_hpa));

	/*
	 * We'll initialize hash lists here
	 */

	for (i = 0; i < KVM_NUM_MMU_PAGES; i++)
		list_create(&vcpu->kvm->arch.mmu_page_hash[i],
			    sizeof(struct kvm_mmu_page), 
			    offsetof(struct kvm_mmu_page, hash_link));

	return alloc_mmu_pages(vcpu);
}

inline uint32_t apic_get_reg(struct kvm_lapic *apic, int reg_off)
{
	return *((uint32_t *) (apic->regs + reg_off));
}

void apic_set_reg(struct kvm_lapic *apic, int reg_off, uint32_t val)
{
	*((uint32_t *) (apic->regs + reg_off)) = val;
}

static inline int apic_x2apic_mode(struct kvm_lapic *apic)
{
	return apic->vcpu->arch.apic_base & X2APIC_ENABLE;
}

static uint32_t apic_get_tmcct(struct kvm_lapic *apic)
{
#ifdef XXX
	ktime_t remaining;
	s64 ns;
	uint32_t tmcct;

	ASSERT(apic != NULL);

	/* if initial count is 0, current count should also be 0 */
	if (apic_get_reg(apic, APIC_TMICT) == 0)
		return 0;

	remaining = hrtimer_get_remaining(&apic->lapic_timer.timer);
	if (ktime_to_ns(remaining) < 0)
		remaining = ktime_set(0, 0);
	ns = mod_64(ktime_to_ns(remaining), apic->lapic_timer.period);
	tmcct = div64_uint64_t(ns,
			 (APIC_BUS_CYCLE_NS * apic->divide_count));

	return tmcct;
#else
	return 0;
#endif /*XXX*/
}

extern unsigned long kvm_rip_read(struct kvm_vcpu *vcpu);

static void __report_tpr_access(struct kvm_lapic *apic, int write)
{
	struct kvm_vcpu *vcpu = apic->vcpu;
	struct kvm_run *run = vcpu->run;

	BT_SET(&vcpu->requests, KVM_REQ_REPORT_TPR_ACCESS);
	run->tpr_access.rip = kvm_rip_read(vcpu);
	run->tpr_access.is_write = write;
}

static inline void report_tpr_access(struct kvm_lapic *apic, int write)
{
	if (apic->vcpu->arch.tpr_access_reporting)
		__report_tpr_access(apic, write);
}

extern void apic_update_ppr(struct kvm_lapic *apic);

static uint32_t __apic_read(struct kvm_lapic *apic, unsigned int offset)
{
	uint32_t val = 0;

	if (offset >= LAPIC_MMIO_LENGTH)
		return 0;

	switch (offset) {
	case APIC_ID:
		if (apic_x2apic_mode(apic))
			val = kvm_apic_id(apic);
		else
			val = kvm_apic_id(apic) << 24;
		break;
	case APIC_ARBPRI:
		cmn_err(CE_WARN,  "Access APIC ARBPRI register which is for P6\n");
		break;

	case APIC_TMCCT:	/* Timer CCR */
		val = apic_get_tmcct(apic);
		break;

	case APIC_TASKPRI:
		report_tpr_access(apic, 0);
		/* fall thru */
	default:
		apic_update_ppr(apic);
		val = apic_get_reg(apic, offset);
		break;
	}

	return val;
}

static inline struct kvm_lapic *to_lapic(struct kvm_io_device *dev)
{
	return container_of(dev, struct kvm_lapic, dev);
}

static int apic_reg_read(struct kvm_lapic *apic, uint32_t offset, int len,
		void *data)
{
	unsigned char alignment = offset & 0xf;
	uint32_t result;
	/* this bitmask has a bit cleared for each reserver register */
	static const uint64_t rmask = 0x43ff01ffffffe70cULL;

	if ((alignment + len) > 4) {
		return 1;
	}

	if (offset > 0x3f0 || !(rmask & (1ULL << (offset >> 4)))) {
		return 1;
	}

	result = __apic_read(apic, offset & ~0xf);
#ifdef XXX
	trace_kvm_apic_read(offset, result);
#endif /*XXX*/

	switch (len) {
	case 1:
	case 2:
	case 4:
		memcpy(data, (char *)&result + alignment, len);
		break;
	default:
		cmn_err(CE_WARN, "Local APIC read with len = %x, should be 1,2, or 4 instead\n", len);
		break;
	}
	return 0;
}

inline int apic_hw_enabled(struct kvm_lapic *apic)
{
	return (apic)->vcpu->arch.apic_base & MSR_IA32_APICBASE_ENABLE;
}

static int apic_mmio_in_range(struct kvm_lapic *apic, gpa_t addr)
{
	return apic_hw_enabled(apic) &&
	    addr >= apic->base_address &&
	    addr < apic->base_address + LAPIC_MMIO_LENGTH;
}

static int apic_mmio_read(struct kvm_io_device *this,
			   gpa_t address, int len, void *data)
{
	struct kvm_lapic *apic = to_lapic(this);
	uint32_t offset = address - apic->base_address;

	if (!apic_mmio_in_range(apic, address))
		return -EOPNOTSUPP;

	apic_reg_read(apic, offset, len, data);

	return 0;
}

#define LVT_MASK	\
	(APIC_LVT_MASKED | APIC_SEND_PENDING | APIC_VECTOR_MASK)

#define LINT_MASK	\
	(LVT_MASK | APIC_MODE_MASK | APIC_INPUT_POLARITY | \
	 APIC_LVT_REMOTE_IRR | APIC_LVT_LEVEL_TRIGGER)

static unsigned int apic_lvt_mask[APIC_LVT_NUM] = {
	LVT_MASK | APIC_LVT_TIMER_PERIODIC,	/* LVTT */
	LVT_MASK | APIC_MODE_MASK,	/* LVTTHMR */
	LVT_MASK | APIC_MODE_MASK,	/* LVTPC */
	LINT_MASK, LINT_MASK,	/* LVT0-1 */
	LVT_MASK		/* LVTERR */
};

static void start_apic_timer(struct kvm_lapic *apic)
{
#ifdef XXX
	ktime_t now = apic->lapic_timer.timer.base->get_time();

	apic->lapic_timer.period = (uint64_t)apic_get_reg(apic, APIC_TMICT) *
		    APIC_BUS_CYCLE_NS * apic->divide_count;
	atomic_set(&apic->lapic_timer.pending, 0);

	if (!apic->lapic_timer.period)
		return;
	/*
	 * Do not allow the guest to program periodic timers with small
	 * interval, since the hrtimers are not throttled by the host
	 * scheduler.
	 */
	if (apic_lvtt_period(apic)) {
		if (apic->lapic_timer.period < NSEC_PER_MSEC/2)
			apic->lapic_timer.period = NSEC_PER_MSEC/2;
	}

	hrtimer_start(&apic->lapic_timer.timer,
		      ktime_add_ns(now, apic->lapic_timer.period),
		      HRTIMER_MODE_ABS);

#endif /*XXX*/
}

inline static int kvm_is_dm_lowest_prio(struct kvm_lapic_irq *irq)
{
#ifdef CONFIG_IA64
	return irq->delivery_mode ==
		(IOSAPIC_LOWEST_PRIORITY << IOSAPIC_DELIVERY_SHIFT);
#else
	return irq->delivery_mode == APIC_DM_LOWEST;
#endif
}

#define VEC_POS(v) ((v) & (32 - 1))
#define REG_POS(v) (((v) >> 5) << 4)

static inline void apic_clear_vector(int vec, caddr_t bitmap)
{
	BT_CLEAR((bitmap) + REG_POS(vec), VEC_POS(vec));
}

void kvm_vcpu_kick(struct kvm_vcpu *vcpu)
{
#ifdef XXX
	int me;
	int cpu = vcpu->cpu;

	if (waitqueue_active(&vcpu->wq)) {
		wake_up_interruptible(&vcpu->wq);
		++vcpu->stat.halt_wakeup;
	}

	me = get_cpu();
	if (cpu != me && (unsigned)cpu < nr_cpu_ids && cpu_online(cpu))
		if (!test_and_set_bit(KVM_REQ_KICK, &vcpu->requests))
			smp_send_reschedule(cpu);
	put_cpu();
#endif /*XXX*/
}

void kvm_inject_nmi(struct kvm_vcpu *vcpu)
{
	vcpu->arch.nmi_pending = 1;
}

static inline void apic_set_vector(int vec, caddr_t bitmap)
{
	BT_SET((bitmap) + REG_POS(vec), VEC_POS(vec));
}

static inline int apic_test_and_set_vector(int vec, caddr_t bitmap)
{
#ifdef XXX
	return test_and_set_bit(VEC_POS(vec), (bitmap) + REG_POS(vec));
#else
	if (BT_TEST((bitmap) + REG_POS(vec), VEC_POS(vec))) {
		BT_SET((bitmap) + REG_POS(vec), VEC_POS(vec));
		return 1;
	} else
		return 0;
#endif /*XXX*/
}


static inline int apic_test_and_set_irr(int vec, struct kvm_lapic *apic)
{
	apic->irr_pending = 1;
	return apic_test_and_set_vector(vec, apic->regs + APIC_IRR);
}

inline int  apic_sw_enabled(struct kvm_lapic *apic)
{
	return apic_get_reg(apic, APIC_SPIV) & APIC_SPIV_APIC_ENABLED;
}

static inline int apic_enabled(struct kvm_lapic *apic)
{
	return apic_sw_enabled(apic) &&	apic_hw_enabled(apic);
}

/*
 * Add a pending IRQ into lapic.
 * Return 1 if successfully added and 0 if discarded.
 */
static int __apic_accept_irq(struct kvm_lapic *apic, int delivery_mode,
			     int vector, int level, int trig_mode)
{
	int result = 0;
	struct kvm_vcpu *vcpu = apic->vcpu;

	switch (delivery_mode) {
	case APIC_DM_LOWEST:
		vcpu->arch.apic_arb_prio++;
	case APIC_DM_FIXED:
		/* FIXME add logic for vcpu on reset */
		if (!apic_enabled(apic))
			break;

		if (trig_mode) {
			apic_set_vector(vector, apic->regs + APIC_TMR);
		} else
			apic_clear_vector(vector, apic->regs + APIC_TMR);

		result = !apic_test_and_set_irr(vector, apic);
		if (!result) {
			break;
		}

		kvm_vcpu_kick(vcpu);
		break;

	case APIC_DM_REMRD:
		break;

	case APIC_DM_SMI:
		break;

	case APIC_DM_NMI:
		result = 1;
		kvm_inject_nmi(vcpu);
		kvm_vcpu_kick(vcpu);
		break;

	case APIC_DM_INIT:
		if (level) {
			result = 1;
			vcpu->arch.mp_state = KVM_MP_STATE_INIT_RECEIVED;
			kvm_vcpu_kick(vcpu);
		}
		break;

	case APIC_DM_STARTUP:
		if (vcpu->arch.mp_state == KVM_MP_STATE_INIT_RECEIVED) {
			result = 1;
			vcpu->arch.sipi_vector = vector;
			vcpu->arch.mp_state = KVM_MP_STATE_SIPI_RECEIVED;
			kvm_vcpu_kick(vcpu);
		}
		break;

	case APIC_DM_EXTINT:
		/*
		 * Should only be called by kvm_apic_local_deliver() with LVT0,
		 * before NMI watchdog was enabled. Already handled by
		 * kvm_apic_accept_pic_intr().
		 */
		break;

	default:
		break;
	}
	return result;
}

int kvm_apic_set_irq(struct kvm_vcpu *vcpu, struct kvm_lapic_irq *irq)
{
	struct kvm_lapic *apic = vcpu->arch.apic;

	return __apic_accept_irq(apic, irq->delivery_mode, irq->vector,
			irq->level, irq->trig_mode);
}

int kvm_irq_delivery_to_apic(struct kvm *kvm, struct kvm_lapic *src,
		struct kvm_lapic_irq *irq)
{
	int i, r = -1;
	struct kvm_vcpu *vcpu, *lowest = NULL;

	if (irq->dest_mode == 0 && irq->dest_id == 0xff &&
			kvm_is_dm_lowest_prio(irq))
		cmn_err(CE_NOTE, "kvm: apic: phys broadcast and lowest prio\n");

#ifdef XXX
	kvm_for_each_vcpu(i, vcpu, kvm) {
		if (!kvm_apic_present(vcpu))
			continue;

		if (!kvm_apic_match_dest(vcpu, src, irq->shorthand,
					irq->dest_id, irq->dest_mode))
			continue;

		if (!kvm_is_dm_lowest_prio(irq)) {
			if (r < 0)
				r = 0;
			r += kvm_apic_set_irq(vcpu, irq);
		} else {
			if (!lowest)
				lowest = vcpu;
			else if (kvm_apic_compare_prio(vcpu, lowest) < 0)
				lowest = vcpu;
		}
	}
#endif /*XXX*/
	if (lowest)
		r = kvm_apic_set_irq(lowest, irq);

	return r;
}

static void apic_send_ipi(struct kvm_lapic *apic)
{
	uint32_t icr_low = apic_get_reg(apic, APIC_ICR);
	uint32_t icr_high = apic_get_reg(apic, APIC_ICR2);
	struct kvm_lapic_irq irq;

	irq.vector = icr_low & APIC_VECTOR_MASK;
	irq.delivery_mode = icr_low & APIC_MODE_MASK;
	irq.dest_mode = icr_low & APIC_DEST_MASK;
	irq.level = icr_low & APIC_INT_ASSERT;
	irq.trig_mode = icr_low & APIC_INT_LEVELTRIG;
	irq.shorthand = icr_low & APIC_SHORT_MASK;
	if (apic_x2apic_mode(apic))
		irq.dest_id = icr_high;
	else
		irq.dest_id = GET_APIC_DEST_FIELD(icr_high);

#ifdef XXX
	trace_kvm_apic_ipi(icr_low, irq.dest_id);

#endif /*XXX*/

	kvm_irq_delivery_to_apic(apic->vcpu->kvm, apic, &irq);
}

static void update_divide_count(struct kvm_lapic *apic)
{
	uint32_t tmp1, tmp2, tdcr;

	tdcr = apic_get_reg(apic, APIC_TDCR);
	tmp1 = tdcr & 0xf;
	tmp2 = ((tmp1 & 0x3) | ((tmp1 & 0x8) >> 1)) + 1;
	apic->divide_count = 0x1 << (tmp2 & 0x7);
}

static void apic_set_tpr(struct kvm_lapic *apic, uint32_t tpr)
{
	apic_set_reg(apic, APIC_TASKPRI, tpr);
	apic_update_ppr(apic);
}


static int ioapic_service(struct kvm_ioapic *ioapic, unsigned int idx)
{
	union kvm_ioapic_redirect_entry *pent;
	int injected = -1;
#ifdef XXX

	pent = &ioapic->redirtbl[idx];

	if (!pent->fields.mask) {
		injected = ioapic_deliver(ioapic, idx);
		if (injected && pent->fields.trig_mode == IOAPIC_LEVEL_TRIG)
			pent->fields.remote_irr = 1;
	}
#endif /*XXX*/
	return injected;
}

static void __kvm_ioapic_update_eoi(struct kvm_ioapic *ioapic, int vector,
				     int trigger_mode)
{
	int i;

#ifdef XXX
	for (i = 0; i < IOAPIC_NUM_PINS; i++) {
		union kvm_ioapic_redirect_entry *ent = &ioapic->redirtbl[i];

		if (ent->fields.vector != vector)
			continue;

		/*
		 * We are dropping lock while calling ack notifiers because ack
		 * notifier callbacks for assigned devices call into IOAPIC
		 * recursively. Since remote_irr is cleared only after call
		 * to notifiers if the same vector will be delivered while lock
		 * is dropped it will be put into irr and will be delivered
		 * after ack notifier returns.
		 */
		mutex_exit(&ioapic->lock);
		kvm_notify_acked_irq(ioapic->kvm, KVM_IRQCHIP_IOAPIC, i);
		mutex_enter(&ioapic->lock);

		if (trigger_mode != IOAPIC_LEVEL_TRIG)
			continue;

		ASSERT(ent->fields.trig_mode == IOAPIC_LEVEL_TRIG);
		ent->fields.remote_irr = 0;
		if (!ent->fields.mask && (ioapic->irr & (1 << i)))
			ioapic_service(ioapic, i);
	}
#endif /*XXX*/
}

void kvm_ioapic_update_eoi(struct kvm *kvm, int vector, int trigger_mode)
{
#ifdef XXX
	struct kvm_ioapic *ioapic = kvm->arch.vioapic;

	smp_rmb();
	if (!BT_TEST(ioapic->handled_vectors, vector))
		return;
	mutex_enter(&ioapic->lock);
	__kvm_ioapic_update_eoi(ioapic, vector, trigger_mode);
	mutex_exit(&ioapic->lock);
#endif /*XXX*/
}

extern inline int apic_find_highest_isr(struct kvm_lapic *apic);

static inline int apic_test_and_clear_vector(int vec, caddr_t bitmap)
{
#ifdef XXX
	return test_and_clear_bit(VEC_POS(vec), (bitmap) + REG_POS(vec));
#else
	if (BT_TEST((bitmap) + REG_POS(vec), VEC_POS(vec))) {
		BT_CLEAR((bitmap) + REG_POS(vec), VEC_POS(vec));
		return (1);
	} else
		return (0);
#endif /*XXX*/

}

static void apic_set_eoi(struct kvm_lapic *apic)
{
	int vector = apic_find_highest_isr(apic);
	int trigger_mode;
	/*
	 * Not every write EOI will has corresponding ISR,
	 * one example is when Kernel check timer on setup_IO_APIC
	 */
	if (vector == -1)
		return;

	apic_clear_vector(vector, apic->regs + APIC_ISR);
	apic_update_ppr(apic);

	if (apic_test_and_clear_vector(vector, apic->regs + APIC_TMR))
		trigger_mode = IOAPIC_LEVEL_TRIG;
	else
		trigger_mode = IOAPIC_EDGE_TRIG;
	if (!(apic_get_reg(apic, APIC_SPIV) & APIC_SPIV_DIRECTED_EOI))
		kvm_ioapic_update_eoi(apic->vcpu->kvm, vector, trigger_mode);
}

static int apic_reg_write(struct kvm_lapic *apic, uint32_t reg, uint32_t val)
{
	int ret = 0;

#ifdef XXX
	trace_kvm_apic_write(reg, val);
#endif /*XXX*/

	switch (reg) {
	case APIC_ID:		/* Local APIC ID */
		if (!apic_x2apic_mode(apic))
			apic_set_reg(apic, APIC_ID, val);
		else
			ret = 1;
		break;

	case APIC_TASKPRI:
		report_tpr_access(apic, 1);
		apic_set_tpr(apic, val & 0xff);
		break;

	case APIC_EOI:
		apic_set_eoi(apic);
		break;

	case APIC_LDR:
		if (!apic_x2apic_mode(apic))
			apic_set_reg(apic, APIC_LDR, val & APIC_LDR_MASK);
		else
			ret = 1;
		break;

	case APIC_DFR:
		if (!apic_x2apic_mode(apic))
			apic_set_reg(apic, APIC_DFR, val | 0x0FFFFFFF);
		else
			ret = 1;
		break;

	case APIC_SPIV: {
		uint32_t mask = 0x3ff;
		if (apic_get_reg(apic, APIC_LVR) & APIC_LVR_DIRECTED_EOI)
			mask |= APIC_SPIV_DIRECTED_EOI;
		apic_set_reg(apic, APIC_SPIV, val & mask);
		if (!(val & APIC_SPIV_APIC_ENABLED)) {
			int i;
			uint32_t lvt_val;

			for (i = 0; i < APIC_LVT_NUM; i++) {
				lvt_val = apic_get_reg(apic,
						       APIC_LVTT + 0x10 * i);
				apic_set_reg(apic, APIC_LVTT + 0x10 * i,
					     lvt_val | APIC_LVT_MASKED);
			}
#ifdef XXX
			atomic_set(&apic->lapic_timer.pending, 0);
#endif
		}
		break;
	}
	case APIC_ICR:
		/* No delay here, so we always clear the pending bit */
		apic_set_reg(apic, APIC_ICR, val & ~(1 << 12));
		apic_send_ipi(apic);
		break;

	case APIC_ICR2:
		if (!apic_x2apic_mode(apic))
			val &= 0xff000000;
		apic_set_reg(apic, APIC_ICR2, val);
		break;

	case APIC_LVT0:
#ifdef XXX
		apic_manage_nmi_watchdog(apic, val);
#endif /*XXX*/
	case APIC_LVTT:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVT1:
	case APIC_LVTERR:
		/* TODO: Check vector */
		if (!apic_sw_enabled(apic))
			val |= APIC_LVT_MASKED;

		val &= apic_lvt_mask[(reg - APIC_LVTT) >> 4];
		apic_set_reg(apic, reg, val);

		break;

	case APIC_TMICT:
#ifdef XXX
		hrtimer_cancel(&apic->lapic_timer.timer);
#endif
		apic_set_reg(apic, APIC_TMICT, val);
		start_apic_timer(apic);
		break;

	case APIC_TDCR:
		if (val & 4)
			cmn_err(CE_WARN, "KVM_WRITE:TDCR %x\n", val);
		apic_set_reg(apic, APIC_TDCR, val);
		update_divide_count(apic);
		break;

	case APIC_ESR:
		if (apic_x2apic_mode(apic) && val != 0) {
			cmn_err(CE_WARN, "KVM_WRITE:ESR not zero %x\n", val);
			ret = 1;
		}
		break;

	case APIC_SELF_IPI:
		if (apic_x2apic_mode(apic)) {
			apic_reg_write(apic, APIC_ICR, 0x40000 | (val & 0xff));
		} else
			ret = 1;
		break;
	default:
		ret = 1;
		break;
	}
	return ret;
}

static int apic_mmio_write(struct kvm_io_device *this,
			    gpa_t address, int len, const void *data)
{
	struct kvm_lapic *apic = to_lapic(this);
	unsigned int offset = address - apic->base_address;
	uint32_t val;

	if (!apic_mmio_in_range(apic, address))
		return -EOPNOTSUPP;

	/*
	 * APIC register must be aligned on 128-bits boundary.
	 * 32/64/128 bits registers must be accessed thru 32 bits.
	 * Refer SDM 8.4.1
	 */
	if (len != 4 || (offset & 0xf)) {
		/* Don't shout loud, $infamous_os would cause only noise. */
		return 0;
	}

	val = *(uint32_t*)data;

	apic_reg_write(apic, offset & 0xff0, val);

	return 0;
}

static const struct kvm_io_device_ops apic_mmio_ops = {
	.read     = apic_mmio_read,
	.write    = apic_mmio_write,
};

int kvm_create_lapic(struct kvm_vcpu *vcpu)
{
	struct kvm_lapic *apic;

	ASSERT(vcpu != NULL);

	apic = kmem_zalloc(sizeof(*apic), KM_SLEEP);
	if (!apic)
		goto nomem;

	vcpu->arch.apic = apic;

	apic->regs_page = kmem_alloc(PAGESIZE, KM_SLEEP);
	if (apic->regs_page == NULL) {
		cmn_err(CE_WARN, "malloc apic regs error for vcpu %x\n",
		       vcpu->vcpu_id);
		goto nomem_free_apic;
	}
	apic->regs = apic->regs_page;
	memset(apic->regs, 0, PAGESIZE);
	apic->vcpu = vcpu;

#ifdef XXX
	hrtimer_init(&apic->lapic_timer.timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_ABS);
	apic->lapic_timer.timer.function = kvm_timer_fn;
	apic->lapic_timer.t_ops = &lapic_timer_ops;
	apic->lapic_timer.kvm = vcpu->kvm;
	apic->lapic_timer.vcpu = vcpu;
#endif /*XXX*/
	apic->base_address = APIC_DEFAULT_PHYS_BASE;
	vcpu->arch.apic_base = APIC_DEFAULT_PHYS_BASE;

	kvm_lapic_reset(vcpu);
	kvm_iodevice_init(&apic->dev, &apic_mmio_ops);

	return 0;
nomem_free_apic:
	kmem_free(apic, PAGESIZE);
nomem:
	return -ENOMEM;
}

int
kvm_arch_vcpu_init(struct kvm_vcpu *vcpu)
{
	void *page;
	struct kvm *kvm;
	int r;

	kvm = vcpu->kvm;

	vcpu->arch.mmu.root_hpa = INVALID_PAGE;
#ifdef XXX
	if (!irqchip_in_kernel(kvm) || kvm_vcpu_is_bsp(vcpu))
		vcpu->arch.mp_state = KVM_MP_STATE_RUNNABLE;
	else
		vcpu->arch.mp_state = KVM_MP_STATE_UNINITIALIZED;
#else
	if (!irqchip_in_kernel(kvm) /* || kvm_vcpu_is_bsp(vcpu) */)
		vcpu->arch.mp_state = KVM_MP_STATE_RUNNABLE;
	else
		vcpu->arch.mp_state = KVM_MP_STATE_UNINITIALIZED;
#endif

	page = kmem_zalloc(PAGESIZE, KM_SLEEP);
	if (!page) {
		r = ENOMEM;
		goto fail;
	}
	vcpu->arch.pio_data = page;

	r = kvm_mmu_create(vcpu);
	if (r < 0)
		goto fail_free_pio_data;

	if (irqchip_in_kernel(kvm)) {
		r = kvm_create_lapic(vcpu);
		if (r < 0)
			goto fail_mmu_destroy;
	}

	vcpu->arch.mce_banks = kmem_zalloc(KVM_MAX_MCE_BANKS * sizeof(uint64_t) * 4,
				       KM_SLEEP);
	if (!vcpu->arch.mce_banks) {
		r = ENOMEM;
		goto fail_free_lapic;
	}
	vcpu->arch.mcg_cap = KVM_MAX_MCE_BANKS;

	return 0;
fail_free_lapic:
#ifdef XXX
	kvm_free_lapic(vcpu);
#endif /*XXX*/
fail_mmu_destroy:
#ifdef XXX
	kvm_mmu_destroy(vcpu);
#endif /*XXX*/
fail_free_pio_data:
	kmem_free(page, PAGESIZE);
	vcpu->arch.pio_data = 0;
fail:
	return r;
}

int
kvm_vcpu_init(struct kvm_vcpu *vcpu, struct kvm *kvm, struct kvm_vcpu_ioc *arg, unsigned id)
{
	struct kvm_run *kvm_run;
	int r;

	mutex_init(&vcpu->mutex, NULL, MUTEX_DRIVER, 0);
	vcpu->cpu = -1;
	vcpu->kvm = kvm;
	vcpu->vcpu_id = id;
#ifdef NOTNOW
	init_waitqueue_head(&vcpu->wq);
#endif
	/* XXX the following assumes returned address is page aligned */
	kvm_run = (struct kvm_run *)kmem_zalloc(PAGESIZE, KM_SLEEP);
	if (!kvm_run) {
		r = ENOMEM;
		goto fail;
	}
	vcpu->run = kvm_run;
	arg->kvm_run_addr = (hat_getpfnum(kas.a_hat, (char *)kvm_run)<<PAGESHIFT)|((uint64_t)kvm_run&PAGEOFFSET);
	arg->kvm_vcpu_addr = (uint64_t)vcpu;

	r = kvm_arch_vcpu_init(vcpu);
	if (r != 0)
		goto fail_free_run;
	return 0;

fail_free_run:
	kmem_free(kvm_run, PAGESIZE);
	vcpu->run = 0;
fail:
	return r;
}

/*
 * For pages for which vmx needs physical addresses,
 * linux allocates pages from an area that maps virtual
 * addresses 1-1 with physical memory.  In this way,
 * translating virtual to physical just involves subtracting
 * the start of the area from the virtual address.
 * This solaris version uses kmem_alloc, so there is no
 * direct mapping of virtual to physical.  We'll change this
 * later if performance is an issue.  For now, we'll use
 * hat_getpfnum() to do the conversion.  Also note that
 * we're assuming 64-bit address space (we won't run on
 * 32-bit hardware).
 */

uint64_t kvm_va2pa(caddr_t va)
{
	uint64_t pa;

	pa = (hat_getpfnum(kas.a_hat, va)<<PAGESHIFT)|((uint64_t)va&PAGEOFFSET);
	return (pa);
}

#ifdef XXX
unsigned long *vmx_io_bitmap_a;
unsigned long *vmx_io_bitmap_b;
unsigned long *vmx_msr_bitmap_legacy;
unsigned long *vmx_msr_bitmap_longmode;
#else
/* make these arrays to try to force into low 4GB memory...*/
/* also need to be aligned... */
__attribute__((__aligned__(PAGESIZE)))unsigned long vmx_io_bitmap_a[PAGESIZE/sizeof(unsigned long)];
__attribute__((__aligned__(PAGESIZE)))unsigned long vmx_io_bitmap_b[PAGESIZE/sizeof(unsigned long)];
__attribute__((__aligned__(PAGESIZE)))unsigned long vmx_msr_bitmap_legacy[PAGESIZE/sizeof(unsigned long)];
__attribute__((__aligned__(PAGESIZE)))unsigned long vmx_msr_bitmap_longmode[PAGESIZE/sizeof(unsigned long)];
#endif /*XXX*/


static void vmcs_write16(unsigned long field, uint16_t value)
{
	vmcs_writel(field, value);
}

static void vmcs_write32(unsigned long field, uint32_t value)
{
	vmcs_writel(field, value);
}

static void vmcs_write64(unsigned long field, uint64_t value)
{
	vmcs_writel(field, value);
#ifndef CONFIG_X86_64
	asm volatile ("");
	vmcs_writel(field+1, value >> 32);
#endif
}

extern int enable_ept;
extern int enable_unrestricted_guest;
extern int emulate_invalid_guest_state;

static int bypass_guest_pf = 1;

extern void vmcs_clear(struct vmcs *vmcs);
extern void vmx_vcpu_load(struct kvm_vcpu *vcpu, int cpu);
extern void vmx_vcpu_put(struct kvm_vcpu *vcpu);

extern int vmx_vcpu_setup(struct vcpu_vmx *vmx);
extern int enable_vpid;

extern ulong_t *vmx_vpid_bitmap;
extern kmutex_t vmx_vpid_lock;

static void allocate_vpid(struct vcpu_vmx *vmx)
{
	int vpid;

	vmx->vpid = 0;
	if (!enable_vpid)
		return;
	mutex_enter(&vmx_vpid_lock);
	vpid = bt_availbit(vmx_vpid_bitmap, VMX_NR_VPIDS);
	if (vpid < VMX_NR_VPIDS) {
		vmx->vpid = vpid;
		BT_SET(vmx_vpid_bitmap, vpid);
	}
	mutex_exit(&vmx_vpid_lock);
}

#ifdef XXX
static int alloc_identity_pagetable(struct kvm *kvm)
{
	struct kvm_userspace_memory_region kvm_userspace_mem;
	int r = 0;

	mutex_enter(&kvm->slots_lock);
	if (kvm->arch.ept_identity_pagetable)
		goto out;
	kvm_userspace_mem.slot = IDENTITY_PAGETABLE_PRIVATE_MEMSLOT;
	kvm_userspace_mem.flags = 0;
	kvm_userspace_mem.guest_phys_addr =
		kvm->arch.ept_identity_map_addr;
	kvm_userspace_mem.memory_size = PAGESIZE;
	r = __kvm_set_memory_region(kvm, &kvm_userspace_mem, 0);
	if (r)
		goto out;

	kvm->arch.ept_identity_pagetable = gfn_to_page(kvm,
			kvm->arch.ept_identity_map_addr >> PAGESHIFT);
out:
	mutex_exit(&kvm->slots_lock);
	return r;
}

#endif /*XXX*/

struct kvm_vcpu *
vmx_create_vcpu(struct kvm *kvm, struct kvm_vcpu_ioc *arg, unsigned int id)
{
	int err;
	struct vcpu_vmx *vmx = kmem_cache_alloc(kvm_vcpu_cache, KM_SLEEP);
	int cpu;

	if (!vmx)
		return NULL;

	allocate_vpid(vmx);
	err = kvm_vcpu_init(&vmx->vcpu, kvm, arg, id);
	if (err) {
#ifdef NOTNOW
		goto free_vcpu;
#else
		kmem_free(vmx, sizeof(struct vcpu_vmx));
		return NULL;
	}
#endif /*NOTNOW*/

	vmx->guest_msrs = kmem_alloc(PAGESIZE, KM_SLEEP);
	if (!vmx->guest_msrs) {
		return NULL;  /* XXX - need cleanup here */
	}

	vmx->vmcs = kmem_zalloc(PAGESIZE, KM_SLEEP);
	if (!vmx->vmcs) {
		kmem_free(vmx, sizeof(struct vcpu_vmx));
		vmx = NULL;
		return NULL;
	}


	kpreempt_disable();

	cpu = curthread->t_cpu->cpu_seqid;

	cmn_err(CE_NOTE, "vmcs revision_id = %x\n", vmcs_config.revision_id);
	vmx->vmcs->revision_id = vmcs_config.revision_id;

	vmcs_clear(vmx->vmcs);

	err = vmx_vcpu_setup(vmx);

	vmx_vcpu_load(&vmx->vcpu, cpu);

	vmx_vcpu_put(&vmx->vcpu);
	kpreempt_enable();
	if (err)
		vmx->vmcs = NULL;
	if (vm_need_virtualize_apic_accesses(kvm))
#ifdef XXX
		if (alloc_apic_access_page(kvm) != 0)
#endif /*XXX*/
			goto free_vmcs;

#ifdef XXX
	/*
	 * XXX For right now, we don't implement ept
	 */
	if (enable_ept) {
		if (!kvm->arch.ept_identity_map_addr)
			kvm->arch.ept_identity_map_addr =
				VMX_EPT_IDENTITY_PAGETABLE_ADDR;
		if (alloc_identity_pagetable(kvm) != 0)
			goto free_vmcs;
	}
#endif /*XXX*/

	return &vmx->vcpu;

free_vmcs:
	kmem_free(vmx->vmcs, PAGESIZE);
	vmx->vmcs = 0;
#ifdef XXX
free_msrs:
	kfree(vmx->guest_msrs);
uninit_vcpu:
	kvm_vcpu_uninit(&vmx->vcpu);
free_vcpu:
	kmem_cache_free(kvm_vcpu_cache, vmx);
	return NULL;
#endif /*XXX*/
}

struct kvm_vcpu *
kvm_arch_vcpu_create(struct kvm *kvm, struct kvm_vcpu_ioc *arg, unsigned int id)
{
	/* for right now, assume always on x86 */
	/* later, if needed, we'll add something here */
	/* to call architecture dependent routine */
	return vmx_create_vcpu(kvm, arg, id);
}

extern int enable_ept;

static void update_exception_bitmap(struct kvm_vcpu *vcpu)
{
	uint32_t eb;

	eb = (1u << PF_VECTOR) | (1u << UD_VECTOR) | (1u << MC_VECTOR) |
	     (1u << NM_VECTOR) | (1u << DB_VECTOR);
#ifdef XXX
	if ((vcpu->guest_debug &
	     (KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP)) ==
	    (KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP))
		eb |= 1u << BP_VECTOR;
#endif /*XXX*/
	if (to_vmx(vcpu)->rmode.vm86_active)
		eb = ~0;
	if (enable_ept)
		eb &= ~(1u << PF_VECTOR); /* bypass_guest_pf = 0 */
	if (vcpu->fpu_active)
		eb &= ~(1u << NM_VECTOR);
	vmcs_write32(EXCEPTION_BITMAP, eb);
}

int kvm_apic_id(struct kvm_lapic *apic)
{
	return (apic_get_reg(apic, APIC_ID) >> 24) & 0xff;
}


void kvm_lapic_set_base(struct kvm_vcpu *vcpu, uint64_t value)
{
	struct kvm_lapic *apic = vcpu->arch.apic;

	if (!apic) {
		value |= MSR_IA32_APICBASE_BSP;
		vcpu->arch.apic_base = value;
		return;
	}

#ifdef XXX
	if (!kvm_vcpu_is_bsp(apic->vcpu))
		value &= ~MSR_IA32_APICBASE_BSP;
#endif /*XXX*/

	vcpu->arch.apic_base = value;
	if (apic_x2apic_mode(apic)) {
		uint32_t id = kvm_apic_id(apic);
		uint32_t ldr = ((id & ~0xf) << 16) | (1 << (id & 0xf));
		apic_set_reg(apic, APIC_LDR, ldr);
	}
	apic->base_address = apic->vcpu->arch.apic_base &
			     MSR_IA32_APICBASE_BASE;

}

uint64_t kvm_get_apic_base(struct kvm_vcpu *vcpu)
{
	if (irqchip_in_kernel(vcpu->kvm))
		return vcpu->arch.apic_base;
	else
		return vcpu->arch.apic_base;
}


void kvm_set_apic_base(struct kvm_vcpu *vcpu, uint64_t data)
{
	/* TODO: reserve bits check */
	if (irqchip_in_kernel(vcpu->kvm))
		kvm_lapic_set_base(vcpu, data);
	else
		vcpu->arch.apic_base = data;
}

void kvm_set_cr8(struct kvm_vcpu *vcpu, unsigned long cr8)
{
	if (cr8 & CR8_RESERVED_BITS) {
		kvm_inject_gp(vcpu, 0);
		return;
	}
#ifdef XXX
	if (irqchip_in_kernel(vcpu->kvm))
		kvm_lapic_set_tpr(vcpu, cr8);
	else
#endif /*XXX*/
		vcpu->arch.cr8 = cr8;
}

int is_paging(struct kvm_vcpu *vcpu)
{
#ifdef XXX	
	return kvm_getcr0_bits(vcpu, X86_CR0_PG);
#else
	return 0;
#endif /*XXX*/
}

void vmx_set_cr4(struct kvm_vcpu *vcpu, unsigned long cr4)
{
	unsigned long hw_cr4 = cr4 | (to_vmx(vcpu)->rmode.vm86_active ?
				      KVM_RMODE_VM_CR4_ALWAYS_ON : KVM_PMODE_VM_CR4_ALWAYS_ON);

	vcpu->arch.cr4 = cr4;
	if (enable_ept) {
		if (!is_paging(vcpu)) {
			hw_cr4 &= ~X86_CR4_PAE;
			hw_cr4 |= X86_CR4_PSE;
		} else if (!(cr4 & X86_CR4_PAE)) {
			hw_cr4 &= ~X86_CR4_PAE;
		}
	}

	vmcs_writel(CR4_READ_SHADOW, cr4);
	vmcs_writel(GUEST_CR4, hw_cr4);
}

static void ept_update_paging_mode_cr0(unsigned long *hw_cr0,
					unsigned long cr0,
					struct kvm_vcpu *vcpu)
{
	if (!(cr0 & X86_CR0_PG)) {
		/* From paging/starting to nonpaging */
		vmcs_write32(CPU_BASED_VM_EXEC_CONTROL,
			     vmcs_read32(CPU_BASED_VM_EXEC_CONTROL) |
			     (CPU_BASED_CR3_LOAD_EXITING |
			      CPU_BASED_CR3_STORE_EXITING));
		vcpu->arch.cr0 = cr0;
		vmx_set_cr4(vcpu, kvm_read_cr4(vcpu));
	} else if (!is_paging(vcpu)) {
		/* From nonpaging to paging */
		vmcs_write32(CPU_BASED_VM_EXEC_CONTROL,
			     vmcs_read32(CPU_BASED_VM_EXEC_CONTROL) &
			     ~(CPU_BASED_CR3_LOAD_EXITING |
			       CPU_BASED_CR3_STORE_EXITING));
		vcpu->arch.cr0 = cr0;
		vmx_set_cr4(vcpu, kvm_read_cr4(vcpu));
	}

	if (!(cr0 & X86_CR0_WP))
		*hw_cr0 &= ~X86_CR0_WP;
}

#define VMX_SEGMENT_FIELD(seg)					\
	[VCPU_SREG_##seg] = {                                   \
		.selector = GUEST_##seg##_SELECTOR,		\
		.base = GUEST_##seg##_BASE,		   	\
		.limit = GUEST_##seg##_LIMIT,		   	\
		.ar_bytes = GUEST_##seg##_AR_BYTES,	   	\
	}

static struct kvm_vmx_segment_field {
	unsigned selector;
	unsigned base;
	unsigned limit;
	unsigned ar_bytes;
} kvm_vmx_segment_fields[] = {
	VMX_SEGMENT_FIELD(CS),
	VMX_SEGMENT_FIELD(DS),
	VMX_SEGMENT_FIELD(ES),
	VMX_SEGMENT_FIELD(FS),
	VMX_SEGMENT_FIELD(GS),
	VMX_SEGMENT_FIELD(SS),
	VMX_SEGMENT_FIELD(TR),
	VMX_SEGMENT_FIELD(LDTR),
};

static void fix_pmode_dataseg(int seg, struct kvm_save_segment *save)
{
	struct kvm_vmx_segment_field *sf = &kvm_vmx_segment_fields[seg];

	if (vmcs_readl(sf->base) == save->base && (save->base & AR_S_MASK)) {
		vmcs_write16(sf->selector, save->selector);
		vmcs_writel(sf->base, save->base);
		vmcs_write32(sf->limit, save->limit);
		vmcs_write32(sf->ar_bytes, save->ar);
	} else {
		uint32_t dpl = (vmcs_read16(sf->selector) & SELECTOR_RPL_MASK)
			<< AR_DPL_SHIFT;
		vmcs_write32(sf->ar_bytes, 0x93 | dpl);
	}
}

static void enter_pmode(struct kvm_vcpu *vcpu)
{
	unsigned long flags;
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	vmx->emulation_required = 1;
	vmx->rmode.vm86_active = 0;

	vmcs_writel(GUEST_TR_BASE, vmx->rmode.tr.base);
	vmcs_write32(GUEST_TR_LIMIT, vmx->rmode.tr.limit);
	vmcs_write32(GUEST_TR_AR_BYTES, vmx->rmode.tr.ar);

	flags = vmcs_readl(GUEST_RFLAGS);
	flags &= RMODE_GUEST_OWNED_EFLAGS_BITS;
	flags |= vmx->rmode.save_rflags & ~RMODE_GUEST_OWNED_EFLAGS_BITS;
	vmcs_writel(GUEST_RFLAGS, flags);

	vmcs_writel(GUEST_CR4, (vmcs_readl(GUEST_CR4) & ~X86_CR4_VME) |
			(vmcs_readl(CR4_READ_SHADOW) & X86_CR4_VME));

	update_exception_bitmap(vcpu);

	if (emulate_invalid_guest_state)
		return;

	fix_pmode_dataseg(VCPU_SREG_ES, &vmx->rmode.es);
	fix_pmode_dataseg(VCPU_SREG_DS, &vmx->rmode.ds);
	fix_pmode_dataseg(VCPU_SREG_GS, &vmx->rmode.gs);
	fix_pmode_dataseg(VCPU_SREG_FS, &vmx->rmode.fs);

	vmcs_write16(GUEST_SS_SELECTOR, 0);
	vmcs_write32(GUEST_SS_AR_BYTES, 0x93);

	vmcs_write16(GUEST_CS_SELECTOR,
		     vmcs_read16(GUEST_CS_SELECTOR) & ~SELECTOR_RPL_MASK);
	vmcs_write32(GUEST_CS_AR_BYTES, 0x9b);
}

static gva_t rmode_tss_base(struct kvm *kvm)
{
	if (!kvm->arch.tss_addr) {
		struct kvm_memslots *slots;
		gfn_t base_gfn;

#ifdef XXX
		slots = rcu_dereference(kvm->memslots);
#else
		slots = kvm->memslots;
#endif /*XXX*/
		base_gfn = kvm->memslots->memslots[0].base_gfn +
				 kvm->memslots->memslots[0].npages - 3;
		return base_gfn << PAGESHIFT;
	}
	return kvm->arch.tss_addr;
}

static void fix_rmode_seg(int seg, struct kvm_save_segment *save)
{
	struct kvm_vmx_segment_field *sf = &kvm_vmx_segment_fields[seg];

	save->selector = vmcs_read16(sf->selector);
	save->base = vmcs_readl(sf->base);
	save->limit = vmcs_read32(sf->limit);
	save->ar = vmcs_read32(sf->ar_bytes);
	vmcs_write16(sf->selector, save->base >> 4);
	vmcs_write32(sf->base, save->base & 0xfffff);
	vmcs_write32(sf->limit, 0xffff);
	vmcs_write32(sf->ar_bytes, 0xf3);
}

static int init_rmode_tss(struct kvm *kvm)
{
	gfn_t fn = rmode_tss_base(kvm) >> PAGESHIFT;
	uint16_t data = 0;
	int ret = 0;
	int r;

	r = kvm_clear_guest_page(kvm, fn, 0, PAGESIZE);
	if (r < 0)
		goto out;
	data = TSS_BASE_SIZE + TSS_REDIRECTION_SIZE;
	r = kvm_write_guest_page(kvm, fn++, &data,
			TSS_IOPB_BASE_OFFSET, sizeof(uint16_t));
	if (r < 0)
		goto out;
	r = kvm_clear_guest_page(kvm, fn++, 0, PAGESIZE);
	if (r < 0)
		goto out;
	r = kvm_clear_guest_page(kvm, fn, 0, PAGESIZE);
	if (r < 0)
		goto out;
	data = ~0;
	r = kvm_write_guest_page(kvm, fn, &data,
				 RMODE_TSS_SIZE - 2 * PAGESIZE - 1,
				 sizeof(uint8_t));
	if (r < 0)
		goto out;

	ret = 1;
out:
	return ret;
}

static int init_rmode(struct kvm *kvm)
{
	if (!init_rmode_tss(kvm))
		return 0;
	if (!init_rmode_identity_map(kvm))
		return 0;
	return 1;
}


static void enter_rmode(struct kvm_vcpu *vcpu)
{
	unsigned long flags;
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (enable_unrestricted_guest)
		return;

	vmx->emulation_required = 1;
	vmx->rmode.vm86_active = 1;

	vmx->rmode.tr.base = vmcs_readl(GUEST_TR_BASE);
	vmcs_writel(GUEST_TR_BASE, rmode_tss_base(vcpu->kvm));

	vmx->rmode.tr.limit = vmcs_read32(GUEST_TR_LIMIT);
	vmcs_write32(GUEST_TR_LIMIT, RMODE_TSS_SIZE - 1);

	vmx->rmode.tr.ar = vmcs_read32(GUEST_TR_AR_BYTES);
	vmcs_write32(GUEST_TR_AR_BYTES, 0x008b);

	flags = vmcs_readl(GUEST_RFLAGS);
	vmx->rmode.save_rflags = flags;

	flags |= X86_EFLAGS_IOPL | X86_EFLAGS_VM;

	vmcs_writel(GUEST_RFLAGS, flags);
	vmcs_writel(GUEST_CR4, vmcs_readl(GUEST_CR4) | X86_CR4_VME);
	update_exception_bitmap(vcpu);

	if (emulate_invalid_guest_state)
		goto continue_rmode;

	vmcs_write16(GUEST_SS_SELECTOR, vmcs_readl(GUEST_SS_BASE) >> 4);
	vmcs_write32(GUEST_SS_LIMIT, 0xffff);
	vmcs_write32(GUEST_SS_AR_BYTES, 0xf3);

	vmcs_write32(GUEST_CS_AR_BYTES, 0xf3);
	vmcs_write32(GUEST_CS_LIMIT, 0xffff);
	if (vmcs_readl(GUEST_CS_BASE) == 0xffff0000)
		vmcs_writel(GUEST_CS_BASE, 0xf0000);
	vmcs_write16(GUEST_CS_SELECTOR, vmcs_readl(GUEST_CS_BASE) >> 4);

	fix_rmode_seg(VCPU_SREG_ES, &vmx->rmode.es);
	fix_rmode_seg(VCPU_SREG_DS, &vmx->rmode.ds);
	fix_rmode_seg(VCPU_SREG_GS, &vmx->rmode.gs);
	fix_rmode_seg(VCPU_SREG_FS, &vmx->rmode.fs);

continue_rmode:
	kvm_mmu_reset_context(vcpu);
	init_rmode(vcpu->kvm);
}

void vmx_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned long hw_cr0;

	if (enable_unrestricted_guest)
		hw_cr0 = (cr0 & ~KVM_GUEST_CR0_MASK_UNRESTRICTED_GUEST)
			| KVM_VM_CR0_ALWAYS_ON_UNRESTRICTED_GUEST;
	else
		hw_cr0 = (cr0 & ~KVM_GUEST_CR0_MASK) | KVM_VM_CR0_ALWAYS_ON;

	if (vmx->rmode.vm86_active && (cr0 & X86_CR0_PE))
		enter_pmode(vcpu);

	if (!vmx->rmode.vm86_active && !(cr0 & X86_CR0_PE))
		enter_rmode(vcpu);

#ifdef CONFIG_X86_64
	if (vcpu->arch.efer & EFER_LME) {
#ifdef XXX
		if (!is_paging(vcpu) && (cr0 & X86_CR0_PG))
			enter_lmode(vcpu);
		if (is_paging(vcpu) && !(cr0 & X86_CR0_PG))
			exit_lmode(vcpu);
#endif /*XXX*/
	}
#endif

	if (enable_ept)
		ept_update_paging_mode_cr0(&hw_cr0, cr0, vcpu);

	if (!vcpu->fpu_active)
		hw_cr0 |= X86_CR0_TS | X86_CR0_MP;

	vmcs_writel(CR0_READ_SHADOW, cr0);
	vmcs_writel(GUEST_CR0, hw_cr0);
	vcpu->arch.cr0 = cr0;
}

static void seg_setup(int seg)
{
	struct kvm_vmx_segment_field *sf = &kvm_vmx_segment_fields[seg];
	unsigned int ar;

	vmcs_write16(sf->selector, 0);
	vmcs_writel(sf->base, 0);
	vmcs_write32(sf->limit, 0xffff);

	if (enable_unrestricted_guest) {
		ar = 0x93;
		if (seg == VCPU_SREG_CS)
			ar |= 0x08; /* code segment */
	} else
		ar = 0xf3;

	vmcs_write32(sf->ar_bytes, ar);
}


extern int kvm_write_guest_page(struct kvm *kvm, gfn_t gfn, const void *data,
			 int offset, int len);

unsigned long empty_zero_page[PAGESIZE / sizeof(unsigned long)];

int kvm_clear_guest_page(struct kvm *kvm, gfn_t gfn, int offset, int len)
{
	return kvm_write_guest_page(kvm, gfn, empty_zero_page, offset, len);
}

static int init_rmode_identity_map(struct kvm *kvm)
{
	int i, r, ret;
	pfn_t identity_map_pfn;
	uint32_t tmp;

	if (!enable_ept)
		return 1;
	if ((!kvm->arch.ept_identity_pagetable)) {
		cmn_err(CE_WARN, "EPT: identity-mapping pagetable haven't been allocated!\n");
		return 0;
	}
	if ((kvm->arch.ept_identity_pagetable_done))
		return 1;
	ret = 0;
	identity_map_pfn = kvm->arch.ept_identity_map_addr >> PAGESHIFT;
	r = kvm_clear_guest_page(kvm, identity_map_pfn, 0, PAGESIZE);
	if (r < 0)
		goto out;
#ifdef XXX
	/* Set up identity-mapping pagetable for EPT in real mode */
	for (i = 0; i < PT32_ENT_PER_PAGE; i++) {
		tmp = (i << 22) + (_PAGE_PRESENT | _PAGE_RW | _PAGE_USER |
			_PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_PSE);
		r = kvm_write_guest_page(kvm, identity_map_pfn,
				&tmp, i * sizeof(tmp), sizeof(tmp));
		if (r < 0)
			goto out;
	}
#endif /*XXX*/
	kvm->arch.ept_identity_pagetable_done = 1;
	ret = 1;
out:
	return ret;
}

extern void vmx_set_efer(struct kvm_vcpu *vcpu, uint64_t efer);
extern void kvm_register_write(struct kvm_vcpu *vcpu,
			       enum kvm_reg reg,
			       unsigned long val);
extern ulong kvm_read_cr0(struct kvm_vcpu *vcpu);
extern void setup_msrs(struct vcpu_vmx *vmx);

int vmx_vcpu_reset(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	uint64_t msr;
	int ret, idx;

	vcpu->arch.regs_avail = ~((1 << VCPU_REGS_RIP) | (1 << VCPU_REGS_RSP));
#ifdef XXX
	idx = srcu_read_lock(&vcpu->kvm->srcu);
#endif /*XXX*/

	if (!init_rmode(vmx->vcpu.kvm)) {
		ret = -ENOMEM;
		goto out;
	}
	vmx->rmode.vm86_active = 0;

	vmx->soft_vnmi_blocked = 0;

	vmx->vcpu.arch.regs[VCPU_REGS_RDX] = get_rdx_init_val();
	kvm_set_cr8(&vmx->vcpu, 0);
	msr = 0xfee00000 | MSR_IA32_APICBASE_ENABLE;
#ifdef XXX
	if (kvm_vcpu_is_bsp(&vmx->vcpu))
		msr |= MSR_IA32_APICBASE_BSP;
#endif /*XXX*/
	kvm_set_apic_base(&vmx->vcpu, msr);

#ifdef XXX
	fx_init(&vmx->vcpu);
#endif /*XXX*/

	seg_setup(VCPU_SREG_CS);
	/*
	 * GUEST_CS_BASE should really be 0xffff0000, but VT vm86 mode
	 * insists on having GUEST_CS_BASE == GUEST_CS_SELECTOR << 4.  Sigh.
	 */
#ifdef CONFIG_KVM_APIC_ARCHITECTURE
	if (kvm_vcpu_is_bsp(&vmx->vcpu)) {
		vmcs_write16(GUEST_CS_SELECTOR, 0xf000);
		vmcs_writel(GUEST_CS_BASE, 0x000f0000);
	} else {
#endif /*CONFIG_KVM_APIC_ARCHITECTURE*/
		vmcs_write16(GUEST_CS_SELECTOR, vmx->vcpu.arch.sipi_vector << 8);
		vmcs_writel(GUEST_CS_BASE, vmx->vcpu.arch.sipi_vector << 12);
#ifdef XXX
	}
#endif /*XXX*/
	seg_setup(VCPU_SREG_DS);
	seg_setup(VCPU_SREG_ES);
	seg_setup(VCPU_SREG_FS);
	seg_setup(VCPU_SREG_GS);
	seg_setup(VCPU_SREG_SS);

	vmcs_write16(GUEST_TR_SELECTOR, 0);
	vmcs_writel(GUEST_TR_BASE, 0);
	vmcs_write32(GUEST_TR_LIMIT, 0xffff);
	vmcs_write32(GUEST_TR_AR_BYTES, 0x008b);

	vmcs_write16(GUEST_LDTR_SELECTOR, 0);
	vmcs_writel(GUEST_LDTR_BASE, 0);
	vmcs_write32(GUEST_LDTR_LIMIT, 0xffff);
	vmcs_write32(GUEST_LDTR_AR_BYTES, 0x00082);

	vmcs_write32(GUEST_SYSENTER_CS, 0);
	vmcs_writel(GUEST_SYSENTER_ESP, 0);
	vmcs_writel(GUEST_SYSENTER_EIP, 0);

	vmcs_writel(GUEST_RFLAGS, 0x02);
#ifdef XXX
	if (kvm_vcpu_is_bsp(&vmx->vcpu))
		kvm_rip_write(vcpu, 0xfff0);
	else
		kvm_rip_write(vcpu, 0);
#endif /*XXX*/
	kvm_register_write(vcpu, VCPU_REGS_RSP, 0);

	vmcs_writel(GUEST_DR7, 0x400);

	vmcs_writel(GUEST_GDTR_BASE, 0);
	vmcs_write32(GUEST_GDTR_LIMIT, 0xffff);

	vmcs_writel(GUEST_IDTR_BASE, 0);
	vmcs_write32(GUEST_IDTR_LIMIT, 0xffff);

	vmcs_write32(GUEST_ACTIVITY_STATE, 0);
	vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, 0);
	vmcs_write32(GUEST_PENDING_DBG_EXCEPTIONS, 0);

	/* Special registers */
	vmcs_write64(GUEST_IA32_DEBUGCTL, 0);

	setup_msrs(vmx);

	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);  /* 22.2.1 */

#ifdef XXX
	if (cpu_has_vmx_tpr_shadow()) {
		vmcs_write64(VIRTUAL_APIC_PAGE_ADDR, 0);
		if (vm_need_tpr_shadow(vmx->vcpu.kvm))
			vmcs_write64(VIRTUAL_APIC_PAGE_ADDR,
				page_to_phys(vmx->vcpu.arch.apic->regs_page));
		vmcs_write32(TPR_THRESHOLD, 0);
	}

	if (vm_need_virtualize_apic_accesses(vmx->vcpu.kvm))
		vmcs_write64(APIC_ACCESS_ADDR,
			     page_to_phys(vmx->vcpu.kvm->arch.apic_access_page));
#endif /*XXX*/

	if (vmx->vpid != 0)
		vmcs_write16(VIRTUAL_PROCESSOR_ID, vmx->vpid);

	vmx->vcpu.arch.cr0 = X86_CR0_NW | X86_CR0_CD | X86_CR0_ET;
	vmx_set_cr0(&vmx->vcpu, kvm_read_cr0(vcpu)); /* enter rmode */
	vmx_set_cr4(&vmx->vcpu, 0);
	vmx_set_efer(&vmx->vcpu, 0);
#ifdef XXX
	vmx_fpu_activate(&vmx->vcpu);
#endif /*XXX*/
	update_exception_bitmap(&vmx->vcpu);
#ifdef XXX
	vpid_sync_vcpu_all(vmx);
#endif /*XXX*/

	ret = 0;

	/* HACK: Don't enable emulation on guest boot/reset */
	vmx->emulation_required = 0;

out:
#ifdef XXX
	srcu_read_unlock(&vcpu->kvm->srcu, idx);
#endif /*XXX*/
	return ret;
}

int kvm_arch_vcpu_reset(struct kvm_vcpu *vcpu)
{
	vcpu->arch.nmi_pending = 0;
	vcpu->arch.nmi_injected = 0;

	vcpu->arch.switch_db_regs = 0;
	memset(vcpu->arch.db, 0, sizeof(vcpu->arch.db));
#ifdef XXX
	vcpu->arch.dr6 = DR6_FIXED_1;
	vcpu->arch.dr7 = DR7_FIXED_1;
#endif /*XXX*/
	/*	return kvm_x86_ops->vcpu_reset(vcpu);*/
	return vmx_vcpu_reset(vcpu);
}

extern void vcpu_load(struct kvm_vcpu *vcpu);

static void nonpaging_new_cr3(struct kvm_vcpu *vcpu)
{
}

void mmu_free_roots(struct kvm_vcpu *vcpu);

static void paging_new_cr3(struct kvm_vcpu *vcpu)
{
	cmn_err(CE_NOTE, "%s: cr3 %lx\n", __func__, vcpu->arch.cr3);
	mmu_free_roots(vcpu);
}

static int mmu_topup_memory_cache(struct kvm_mmu_memory_cache *cache,
				  struct kmem_cache *base_cache, int min)
{
	caddr_t obj;

	if (cache->nobjs >= min)
		return 0;
	while (cache->nobjs < ARRAY_SIZE(cache->objects)) {
		obj = kmem_cache_alloc(base_cache, KM_SLEEP);
		if (!obj)
			return -ENOMEM;
		cache->objects[cache->nobjs++] = obj;
	}
	return 0;
}

extern struct kmem_cache *pte_chain_cache;
extern struct kmem_cache *rmap_desc_cache;
extern struct kmem_cache *mmu_page_header_cache;

/*XXX the following is called for tdp (two dimensional hardware paging */
/* we dont support this right now */
static int mmu_topup_memory_cache_page(struct kvm_mmu_memory_cache *cache,
				       int min)
{
	caddr_t page;

	if (cache->nobjs >= min)
		return 0;
	while (cache->nobjs < ARRAY_SIZE(cache->objects)) {
		page = kmem_zalloc(PAGESIZE, KM_SLEEP);
		if (!page)
			return -ENOMEM;
#ifdef XXX
		set_page_private(page, 0);
		cache->objects[cache->nobjs++] = page_address(page);
#else
		cache->objects[cache->nobjs++] = page;
#endif /*XXX*/
	}
	return 0;
}

int mmu_topup_memory_caches(struct kvm_vcpu *vcpu)
{
	int r = 0;

	r = mmu_topup_memory_cache(&vcpu->arch.mmu_pte_chain_cache,
				   pte_chain_cache, 4);
	if (r)
		goto out;
	r = mmu_topup_memory_cache(&vcpu->arch.mmu_rmap_desc_cache,
				   rmap_desc_cache, 4);
	if (r)
		goto out;
	r = mmu_topup_memory_cache_page(&vcpu->arch.mmu_page_cache, 8);
	if (r)
		goto out;
	r = mmu_topup_memory_cache(&vcpu->arch.mmu_page_header_cache,
				   mmu_page_header_cache, 4);
out:
	return r;
}

gfn_t unalias_gfn(struct kvm *kvm, gfn_t gfn);
extern struct kvm_memory_slot *gfn_to_memslot_unaliased(struct kvm *kvm, gfn_t gfn);

struct kvm_memory_slot *gfn_to_memslot(struct kvm *kvm, gfn_t gfn)
{
	gfn = unalias_gfn(kvm, gfn);
	return gfn_to_memslot_unaliased(kvm, gfn);
}

/*
 * Return the pointer to the largepage write count for a given
 * gfn, handling slots that are not large page aligned.
 */
static int *slot_largepage_idx(gfn_t gfn,
			       struct kvm_memory_slot *slot,
			       int level)
{
	unsigned long idx;

	idx = (gfn / KVM_PAGES_PER_HPAGE(level)) -
	      (slot->base_gfn / KVM_PAGES_PER_HPAGE(level));
	return &slot->lpage_info[level - 2][idx].write_count;
}

static int has_wrprotected_page(struct kvm *kvm,
				gfn_t gfn,
				int level)
{
	struct kvm_memory_slot *slot;
	int *largepage_idx;

	gfn = unalias_gfn(kvm, gfn);
	slot = gfn_to_memslot_unaliased(kvm, gfn);
	if (slot) {
		largepage_idx = slot_largepage_idx(gfn, slot, level);
		return *largepage_idx;
	}

	return 1;
}

static int mapping_level(struct kvm_vcpu *vcpu, gfn_t large_gfn)
{
	struct kvm_memory_slot *slot;
	int host_level, level, max_level;
#ifdef XXX
	slot = gfn_to_memslot(vcpu->kvm, large_gfn);
	if (slot && slot->dirty_bitmap)
		return PT_PAGE_TABLE_LEVEL;

	host_level = host_mapping_level(vcpu->kvm, large_gfn);

	if (host_level == PT_PAGE_TABLE_LEVEL)
		return host_level;

	max_level = kvm_x86_ops->get_lpage_level() < host_level ?
		kvm_x86_ops->get_lpage_level() : host_level;

	for (level = PT_DIRECTORY_LEVEL; level <= max_level; ++level)
		if (has_wrprotected_page(vcpu->kvm, large_gfn, level))
			break;

	return level - 1;
#else
	return 0;
#endif /*XXX*/
}

extern unsigned long gfn_to_hva(struct kvm *kvm, gfn_t gfn);

extern int kvm_is_error_hva(unsigned long addr);

extern caddr_t bad_page;
extern inline void get_page(caddr_t page);

#define page_to_pfn(page) (hat_getpfnum(kas.a_hat, page))

static pfn_t hva_to_pfn(struct kvm *kvm, unsigned long addr)
{
	caddr_t page[1];
	int npages;
	pfn_t pfn;

#ifdef XXX
	might_sleep();

	npages = get_user_pages_fast(addr, 1, 1, page);

	if (unlikely(npages != 1)) {
		struct vm_area_struct *vma;

		down_read(&current->mm->mmap_sem);
		vma = find_vma(current->mm, addr);

		if (vma == NULL || addr < vma->vm_start ||
		    !(vma->vm_flags & VM_PFNMAP)) {
			up_read(&current->mm->mmap_sem);
			get_page(bad_page);
			return page_to_pfn(bad_page);
		}

		pfn = ((addr - vma->vm_start) >> PAGESHIFT) + vma->vm_pgoff;
		up_read(&current->mm->mmap_sem);
		BUG_ON(!kvm_is_mmio_pfn(pfn));
	} else
		pfn = page_to_pfn(page[0]);
#else
	pfn = hat_getpfnum(curthread, addr);
#endif /*XXX*/

	return pfn;
}

pfn_t gfn_to_pfn(struct kvm *kvm, gfn_t gfn)
{
	unsigned long addr;

	addr = gfn_to_hva(kvm, gfn);
	if (kvm_is_error_hva(addr)) {
		get_page(bad_page);
		return page_to_pfn(bad_page);
	}

	return hva_to_pfn(kvm, addr);
}

extern pfn_t bad_pfn;

int is_error_pfn(pfn_t pfn)
{
	return pfn == bad_pfn;
}

int is_nx(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.efer & EFER_NX;
}

struct kvm_shadow_walk_iterator {
	uint64_t addr;
	hpa_t shadow_addr;
	int level;
	uint64_t *sptep;
	unsigned index;
};

extern struct kvm_mmu_page * page_header(hpa_t shadow_page, struct kvm *kvm);

pfn_t spte_to_pfn(uint64_t pte)
{
	return (pte & PT64_BASE_ADDR_MASK) >> PAGESHIFT;
}

/*
 * Take gfn and return the reverse mapping to it.
 * Note: gfn must be unaliased before this function get called
 */

unsigned long *gfn_to_rmap(struct kvm *kvm, gfn_t gfn, int level)
{
	struct kvm_memory_slot *slot;
	unsigned long idx;

	slot = gfn_to_memslot(kvm, gfn);
	if (level == PT_PAGE_TABLE_LEVEL)
		return &slot->rmap[gfn - slot->base_gfn];

	idx = (gfn / KVM_PAGES_PER_HPAGE(level)) -
		(slot->base_gfn / KVM_PAGES_PER_HPAGE(level));

	return &slot->lpage_info[level - 2][idx].rmap_pde;
}

void kvm_set_pfn_accessed(pfn_t pfn)
{
#ifdef XXX
	if (!kvm_is_mmio_pfn(pfn))
		mark_page_accessed(pfn_to_page(pfn));
#endif /*XXX*/
}

static void mmu_free_rmap_desc(struct kvm_rmap_desc *rd)
{
	kmem_free(rd, sizeof(struct kvm_rmap_desc));
}

static void rmap_desc_remove_entry(unsigned long *rmapp,
				   struct kvm_rmap_desc *desc,
				   int i,
				   struct kvm_rmap_desc *prev_desc)
{
	int j;

	for (j = RMAP_EXT - 1; !desc->sptes[j] && j > i; --j)
		;
	desc->sptes[i] = desc->sptes[j];
	desc->sptes[j] = NULL;
	if (j != 0)
		return;
	if (!prev_desc && !desc->more)
		*rmapp = (unsigned long)desc->sptes[0];
	else
		if (prev_desc)
			prev_desc->more = desc->more;
		else
			*rmapp = (unsigned long)desc->more | 1;
	mmu_free_rmap_desc(desc);
}

void kvm_set_pfn_dirty(pfn_t pfn)
{
#ifdef XXX
	if (!kvm_is_mmio_pfn(pfn)) {
		struct page *page = pfn_to_page(pfn);
		if (!PageReserved(page))
			SetPageDirty(page);
	}
#endif /*XXX*/
}

int 
is_shadow_present_pte(uint64_t pte)
{
	return pte != shadow_trap_nonpresent_pte
		&& pte != shadow_notrap_nonpresent_pte;
}

static int is_rmap_spte(uint64_t pte)
{
	return is_shadow_present_pte(pte);
}

extern int is_writable_pte(unsigned long pte);

void rmap_remove(struct kvm *kvm, uint64_t *spte)
{
	struct kvm_rmap_desc *desc;
	struct kvm_rmap_desc *prev_desc;
	struct kvm_mmu_page *sp;
	pfn_t pfn;
	unsigned long *rmapp;
	int i;

	if (!is_rmap_spte(*spte))
		return;
	sp = page_header(kvm_va2pa(spte), kvm);
	pfn = spte_to_pfn(*spte);
	if (*spte & shadow_accessed_mask)
		kvm_set_pfn_accessed(pfn);
	if (is_writable_pte(*spte))
		kvm_set_pfn_dirty(pfn);
	rmapp = gfn_to_rmap(kvm, sp->gfns[spte - sp->spt], sp->role.level);
	if (!*rmapp) {
		cmn_err(CE_WARN, "rmap_remove: %p %llx 0->BUG\n", spte, *spte);
	} else if (!(*rmapp & 1)) {
		cmn_err(CE_NOTE, "rmap_remove:  %p %llx 1->0\n", spte, *spte);
		if ((uint64_t *)*rmapp != spte) {
			cmn_err(CE_WARN, "rmap_remove:  %p %llx 1->BUG\n",
			       spte, *spte);
		}
		*rmapp = 0;
	} else {
		desc = (struct kvm_rmap_desc *)(*rmapp & ~1ul);
		prev_desc = NULL;
		while (desc) {
			for (i = 0; i < RMAP_EXT && desc->sptes[i]; ++i)
				if (desc->sptes[i] == spte) {
					rmap_desc_remove_entry(rmapp,
							       desc, i,
							       prev_desc);
					return;
				}
			prev_desc = desc;
			desc = desc->more;
		}
	}
}

static void *mmu_memory_cache_alloc(struct kvm_mmu_memory_cache *mc,
				    size_t size)
{
	void *p;

	p = mc->objects[--mc->nobjs];
	return p;
}

static struct kvm_rmap_desc *mmu_alloc_rmap_desc(struct kvm_vcpu *vcpu)
{
	return mmu_memory_cache_alloc(&vcpu->arch.mmu_rmap_desc_cache,
				      sizeof(struct kvm_rmap_desc));
}


/*
 * Reverse mapping data structures:
 *
 * If rmapp bit zero is zero, then rmapp point to the shadw page table entry
 * that points to page_address(page).
 *
 * If rmapp bit zero is one, (then rmap & ~1) points to a struct kvm_rmap_desc
 * containing more mappings.
 *
 * Returns the number of rmap entries before the spte was added or zero if
 * the spte was not added.
 *
 */
static int rmap_add(struct kvm_vcpu *vcpu, uint64_t *spte, gfn_t gfn)
{
	struct kvm_mmu_page *sp;
	struct kvm_rmap_desc *desc;
	unsigned long *rmapp;
	int i, count = 0;

	if (!is_rmap_spte(*spte))
		return count;
	gfn = unalias_gfn(vcpu->kvm, gfn);
	sp = page_header(kvm_va2pa(spte), vcpu->kvm);
	sp->gfns[spte - sp->spt] = gfn;
	rmapp = gfn_to_rmap(vcpu->kvm, gfn, sp->role.level);
	if (!*rmapp) {
		*rmapp = (unsigned long)spte;
	} else if (!(*rmapp & 1)) {
		desc = mmu_alloc_rmap_desc(vcpu);
		desc->sptes[0] = (uint64_t *)*rmapp;
		desc->sptes[1] = spte;
		*rmapp = (unsigned long)desc | 1;
	} else {
		desc = (struct kvm_rmap_desc *)(*rmapp & ~1ul);
		while (desc->sptes[RMAP_EXT-1] && desc->more) {
			desc = desc->more;
			count += RMAP_EXT;
		}
		if (desc->sptes[RMAP_EXT-1]) {
			desc->more = mmu_alloc_rmap_desc(vcpu);
			desc = desc->more;
		}
		for (i = 0; desc->sptes[i]; ++i)
			;
		desc->sptes[i] = spte;
	}
	return count;
}

int memslot_id(struct kvm *kvm, gfn_t gfn)
{
	int i;
#ifdef XXX
	struct kvm_memslots *slots = rcu_dereference(kvm->memslots);
#else
	struct kvm_memslots *slots = kvm->memslots;
#endif /*XXX*/
	struct kvm_memory_slot *memslot = NULL;

	gfn = unalias_gfn(kvm, gfn);
	for (i = 0; i < slots->nmemslots; ++i) {
		memslot = &slots->memslots[i];

		if (gfn >= memslot->base_gfn
		    && gfn < memslot->base_gfn + memslot->npages)
			break;
	}

	return memslot - slots->memslots;
}


static void page_header_update_slot(struct kvm *kvm, void *pte, gfn_t gfn)
{
	int slot = memslot_id(kvm, gfn);
	struct kvm_mmu_page *sp = page_header(kvm_va2pa(pte), kvm);

	BT_SET(sp->slot_bitmap, slot);
}

void kvm_release_pfn_clean(pfn_t pfn)
{
#ifdef XXX  /*XXX probably just free the page */	
	if (!kvm_is_mmio_pfn(pfn))
		put_page(pfn_to_page(pfn));
#endif /*XXX*/
}

void kvm_release_pfn_dirty(pfn_t pfn)
{
	kvm_set_pfn_dirty(pfn);
	kvm_release_pfn_clean(pfn);
}

extern void mmu_page_remove_parent_pte(struct kvm_mmu_page *sp,
				       uint64_t *parent_pte);

void __set_spte(uint64_t *sptep, uint64_t spte)
{
#ifdef XXX
#ifdef CONFIG_X86_64
	set_64bit((unsigned long *)sptep, spte);
#else
	set_64bit((unsigned long long *)sptep, spte);
#endif
#else
	*sptep = spte;
#endif /*XXX*/
}

extern int tdp_enabled;
extern inline ulong kvm_read_cr0_bits(struct kvm_vcpu *vcpu, ulong mask);

static int is_write_protection(struct kvm_vcpu *vcpu)
{
	return kvm_read_cr0_bits(vcpu, X86_CR0_WP);
}

#define PT_FIRST_AVAIL_BITS_SHIFT 9
#define PT64_SECOND_AVAIL_BITS_SHIFT 52
#define SPTE_HOST_WRITEABLE (1ULL << PT_FIRST_AVAIL_BITS_SHIFT)

static int oos_shadow = 1;

extern unsigned kvm_page_table_hashfn(gfn_t gfn);

static struct kvm_mmu_page *kvm_mmu_lookup_page(struct kvm *kvm, gfn_t gfn)
{
	unsigned index;
	list_t *bucket;
	struct kvm_mmu_page *sp;

	index = kvm_page_table_hashfn(gfn);
	bucket = &kvm->arch.mmu_page_hash[index];
	for (sp = list_head(bucket); sp; sp = list_next(bucket, sp)) {
		if (sp->gfn == gfn && !sp->role.direct
		    && !sp->role.invalid) {
			return sp;
		}
	}
	return NULL;
}

static void mmu_convert_notrap(struct kvm_mmu_page *sp)
{
	int i;
	uint64_t *pt = sp->spt;

	if (shadow_trap_nonpresent_pte == shadow_notrap_nonpresent_pte)
		return;

	for (i = 0; i < PT64_ENT_PER_PAGE; ++i) {
		if (pt[i] == shadow_notrap_nonpresent_pte)
			__set_spte(&pt[i], shadow_trap_nonpresent_pte);
	}
}

extern void kvm_mmu_mark_parents_unsync(struct kvm_vcpu *vcpu,
					struct kvm_mmu_page *sp);

static int kvm_unsync_page(struct kvm_vcpu *vcpu, struct kvm_mmu_page *sp)
{
	unsigned index;
	list_t *bucket;
	struct kvm_mmu_page *s;

	index = kvm_page_table_hashfn(sp->gfn);
	bucket = &vcpu->kvm->arch.mmu_page_hash[index];
	/* don't unsync if pagetable is shadowed with multiple roles */
	/* XXX - need protection here(?) */
	for(s = list_head(bucket); s; s = list_next(bucket, s)) {
		if (s->gfn != sp->gfn || s->role.direct)
			continue;
		if (s->role.word != sp->role.word)
			return 1;
	}
	sp->unsync = 1;

	kvm_mmu_mark_parents_unsync(vcpu, sp);

	mmu_convert_notrap(sp);
	return 0;
}

static int mmu_need_write_protect(struct kvm_vcpu *vcpu, gfn_t gfn,
				  int can_unsync)
{
	struct kvm_mmu_page *shadow;

	shadow = kvm_mmu_lookup_page(vcpu->kvm, gfn);
	if (shadow) {
		if (shadow->role.level != PT_PAGE_TABLE_LEVEL)
			return 1;
		if (shadow->unsync)
			return 0;
		if (can_unsync && oos_shadow)
			return kvm_unsync_page(vcpu, shadow);
		return 1;
	}
	return 0;
}

int set_spte(struct kvm_vcpu *vcpu, uint64_t *sptep,
		    unsigned pte_access, int user_fault,
		    int write_fault, int dirty, int level,
		    gfn_t gfn, pfn_t pfn, int speculative,
		    int can_unsync, int reset_host_protection)
{
	uint64_t spte;
	int ret = 0;

	/*
	 * We don't set the accessed bit, since we sometimes want to see
	 * whether the guest actually used the pte (in order to detect
	 * demand paging).
	 */
	spte = shadow_base_present_pte | shadow_dirty_mask;
	if (!speculative)
		spte |= shadow_accessed_mask;
	if (!dirty)
		pte_access &= ~ACC_WRITE_MASK;
	if (pte_access & ACC_EXEC_MASK)
		spte |= shadow_x_mask;
	else
		spte |= shadow_nx_mask;
	if (pte_access & ACC_USER_MASK)
		spte |= shadow_user_mask;
	if (level > PT_PAGE_TABLE_LEVEL)
		spte |= PT_PAGE_SIZE_MASK;
	if (tdp_enabled)
		spte |= kvm_x86_ops->get_mt_mask(vcpu, gfn,
			kvm_is_mmio_pfn(pfn));

	if (reset_host_protection)
		spte |= SPTE_HOST_WRITEABLE;

	spte |= (uint64_t)pfn << PAGESHIFT;

	if ((pte_access & ACC_WRITE_MASK)
	    || (write_fault && !is_write_protection(vcpu) && !user_fault)) {

		if (level > PT_PAGE_TABLE_LEVEL &&
		    has_wrprotected_page(vcpu->kvm, gfn, level)) {
			ret = 1;
			spte = shadow_trap_nonpresent_pte;
			goto set_pte;
		}

		spte |= PT_WRITABLE_MASK;

		/*
		 * Optimization: for pte sync, if spte was writable the hash
		 * lookup is unnecessary (and expensive). Write protection
		 * is responsibility of mmu_get_page / kvm_sync_page.
		 * Same reasoning can be applied to dirty page accounting.
		 */
		if (!can_unsync && is_writable_pte(*sptep))
			goto set_pte;

		if (mmu_need_write_protect(vcpu, gfn, can_unsync)) {
			ret = 1;
			pte_access &= ~ACC_WRITE_MASK;
			if (is_writable_pte(spte))
				spte &= ~PT_WRITABLE_MASK;
		}
	}

	if (pte_access & ACC_WRITE_MASK)
		mark_page_dirty(vcpu->kvm, gfn);

set_pte:
	__set_spte(sptep, spte);
	return ret;
}

extern int is_large_pte(uint64_t pte);

static void mmu_set_spte(struct kvm_vcpu *vcpu, uint64_t *sptep,
			 unsigned pt_access, unsigned pte_access,
			 int user_fault, int write_fault, int dirty,
			 int *ptwrite, int level, gfn_t gfn,
			 pfn_t pfn, int speculative,
			 int reset_host_protection)
{
	int was_rmapped = 0;
	int was_writable = is_writable_pte(*sptep);
	int rmap_count;

	if (is_rmap_spte(*sptep)) {
		/*
		 * If we overwrite a PTE page pointer with a 2MB PMD, unlink
		 * the parent of the now unreachable PTE.
		 */
		if (level > PT_PAGE_TABLE_LEVEL &&
		    !is_large_pte(*sptep)) {
			struct kvm_mmu_page *child;
			uint64_t pte = *sptep;

			child = page_header(pte & PT64_BASE_ADDR_MASK, vcpu->kvm);
			mmu_page_remove_parent_pte(child, sptep);
		} else if (pfn != spte_to_pfn(*sptep)) {
			rmap_remove(vcpu->kvm, sptep);
		} else
			was_rmapped = 1;
	}

	if (set_spte(vcpu, sptep, pte_access, user_fault, write_fault,
		      dirty, level, gfn, pfn, speculative, 1,
		      reset_host_protection)) {
		if (write_fault)
			*ptwrite = 1;
		kvm_x86_ops->tlb_flush(vcpu);
	}

#ifdef XXX
	if (!was_rmapped && is_large_pte(*sptep))
		++vcpu->kvm->stat.lpages;
#endif /*XXX*/

	page_header_update_slot(vcpu->kvm, sptep, gfn);
	if (!was_rmapped) {
		rmap_count = rmap_add(vcpu, sptep, gfn);
		kvm_release_pfn_clean(pfn);
#ifdef XXX
		if (rmap_count > RMAP_RECYCLE_THRESHOLD)
			rmap_recycle(vcpu, sptep, gfn);
#endif /*XXX*/
	} else {
		if (was_writable)
			kvm_release_pfn_dirty(pfn);
		else
			kvm_release_pfn_clean(pfn);
	}
#ifdef XXX
	if (speculative) {
		vcpu->arch.last_pte_updated = sptep;
		vcpu->arch.last_pte_gfn = gfn;
	}
#endif /*XXX*/
}


static int __direct_map(struct kvm_vcpu *vcpu, gpa_t v, int write,
			int level, gfn_t gfn, pfn_t pfn)
{
	struct kvm_shadow_walk_iterator iterator;
	struct kvm_mmu_page *sp;
	int pt_write = 0;
	gfn_t pseudo_gfn;

#ifdef XXX
	for_each_shadow_entry(vcpu, (uint64_t)gfn << PAGESHIFT, iterator) {
		if (iterator.level == level) {
			mmu_set_spte(vcpu, iterator.sptep, ACC_ALL, ACC_ALL,
				     0, write, 1, &pt_write,
				     level, gfn, pfn, 0, 1);
			++vcpu->stat.pf_fixed;
			break;
		}

		if (*iterator.sptep == shadow_trap_nonpresent_pte) {
			pseudo_gfn = (iterator.addr & PT64_DIR_BASE_ADDR_MASK) >> PAGESHIFT;
			sp = kvm_mmu_get_page(vcpu, pseudo_gfn, iterator.addr,
					      iterator.level - 1,
					      1, ACC_ALL, iterator.sptep);
			if (!sp) {
				kvm_release_pfn_clean(pfn);
				return -ENOMEM;
			}

			__set_spte(iterator.sptep,
				   kvm_va2pa(sp->spt)
				   | PT_PRESENT_MASK | PT_WRITABLE_MASK
				   | shadow_user_mask | shadow_x_mask);
		}
	}
#endif /*XXX*/
	return pt_write;
}

inline void kvm_mmu_free_some_pages(struct kvm_vcpu *vcpu)
{
#ifdef XXX
	if (unlikely(vcpu->kvm->arch.n_free_mmu_pages < KVM_MIN_FREE_MMU_PAGES))
		__kvm_mmu_free_some_pages(vcpu);
#endif /*XXX*/
}

static int tdp_page_fault(struct kvm_vcpu *vcpu, gva_t gpa,
				uint32_t error_code)
{
#ifdef XXX
	pfn_t pfn;
	int r;
	int level;
	gfn_t gfn = gpa >> PAGESHIFT;
	unsigned long mmu_seq;

	ASSERT(vcpu);
	ASSERT(VALID_PAGE(vcpu->arch.mmu.root_hpa));

	r = mmu_topup_memory_caches(vcpu);
	if (r)
		return r;

	level = mapping_level(vcpu, gfn);

	gfn &= ~(KVM_PAGES_PER_HPAGE(level) - 1);

	mmu_seq = vcpu->kvm->mmu_notifier_seq;
	smp_rmb();

	pfn = gfn_to_pfn(vcpu->kvm, gfn);
	if (is_error_pfn(pfn)) {
		kvm_release_pfn_clean(pfn);
		return 1;
	}
	mutex_enter(&vcpu->kvm->mmu_lock);
#ifdef XXX
	if (mmu_notifier_retry(vcpu, mmu_seq))
		goto out_unlock;
#endif /*XXX*/
	kvm_mmu_free_some_pages(vcpu);
	r = __direct_map(vcpu, gpa, error_code & PFERR_WRITE_MASK,
			 level, gfn, pfn);
	mutex_exit(&vcpu->kvm->mmu_lock);

	return r;

out_unlock:
	mutex_exit(&vcpu->kvm->mmu_lock);
	kvm_release_pfn_clean(pfn);
#endif /*XXX*/
	return 0;
}

extern int kvm_mmu_zap_page(struct kvm *kvm, struct kvm_mmu_page *sp);

void mmu_free_roots(struct kvm_vcpu *vcpu)
{
	int i;
	struct kvm_mmu_page *sp;

	if (!VALID_PAGE(vcpu->arch.mmu.root_hpa))
		return;
	mutex_enter(&vcpu->kvm->mmu_lock);
	if (vcpu->arch.mmu.shadow_root_level == PT64_ROOT_LEVEL) {
		hpa_t root = vcpu->arch.mmu.root_hpa;

		sp = page_header(root, vcpu->kvm);
		--sp->root_count;
		if (!sp->root_count && sp->role.invalid)
			kvm_mmu_zap_page(vcpu->kvm, sp);
		vcpu->arch.mmu.root_hpa = INVALID_PAGE;
		mutex_exit(&vcpu->kvm->mmu_lock);
		return;
	}
	for (i = 0; i < 4; ++i) {
		hpa_t root = vcpu->arch.mmu.pae_root[i];

		if (root) {
			root &= PT64_BASE_ADDR_MASK;
			sp = page_header(root, vcpu->kvm);
			--sp->root_count;
			if (!sp->root_count && sp->role.invalid)
				kvm_mmu_zap_page(vcpu->kvm, sp);
		}
		vcpu->arch.mmu.pae_root[i] = INVALID_PAGE;
	}
	mutex_exit(&vcpu->kvm->mmu_lock);
	vcpu->arch.mmu.root_hpa = INVALID_PAGE;
}

static void nonpaging_free(struct kvm_vcpu *vcpu)
{
	mmu_free_roots(vcpu);
}

static void paging_free(struct kvm_vcpu *vcpu)
{
	nonpaging_free(vcpu);
}

static void nonpaging_prefetch_page(struct kvm_vcpu *vcpu,
				    struct kvm_mmu_page *sp)
{
	int i;

	for (i = 0; i < PT64_ENT_PER_PAGE; ++i)
		sp->spt[i] = shadow_trap_nonpresent_pte;
}

static int nonpaging_sync_page(struct kvm_vcpu *vcpu,
			       struct kvm_mmu_page *sp)
{
	return 1;
}

static void nonpaging_invlpg(struct kvm_vcpu *vcpu, gva_t gva)
{
}

int get_ept_level(void)
{
	return VMX_EPT_DEFAULT_GAW + 1;
}

static gpa_t nonpaging_gva_to_gpa(struct kvm_vcpu *vcpu, gva_t vaddr,
				  uint32_t access, uint32_t *error)
{
	if (error)
		*error = 0;
	return vaddr;
}

int is_cpuid_PSE36(void)
{
	return 1;
}

int cpuid_maxphyaddr(struct kvm_vcpu *vcpu)
{
	return 36;  /* from linux.  number of bits, perhaps? */
}

static inline uint64_t rsvd_bits(int s, int e)
{
	return ((1ULL << (e - s + 1)) - 1) << s;
}

static void reset_rsvds_bits_mask(struct kvm_vcpu *vcpu, int level)
{
	struct kvm_mmu *context = &vcpu->arch.mmu;
	int maxphyaddr = cpuid_maxphyaddr(vcpu);
	uint64_t exb_bit_rsvd = 0;

	if (!is_nx(vcpu))
		exb_bit_rsvd = rsvd_bits(63, 63);
	switch (level) {
	case PT32_ROOT_LEVEL:
		/* no rsvd bits for 2 level 4K page table entries */
		context->rsvd_bits_mask[0][1] = 0;
		context->rsvd_bits_mask[0][0] = 0;
		if (is_cpuid_PSE36())
			/* 36bits PSE 4MB page */
			context->rsvd_bits_mask[1][1] = rsvd_bits(17, 21);
		else
			/* 32 bits PSE 4MB page */
			context->rsvd_bits_mask[1][1] = rsvd_bits(13, 21);
		context->rsvd_bits_mask[1][0] = context->rsvd_bits_mask[1][0];
		break;
	case PT32E_ROOT_LEVEL:
		context->rsvd_bits_mask[0][2] =
			rsvd_bits(maxphyaddr, 63) |
			rsvd_bits(7, 8) | rsvd_bits(1, 2);	/* PDPTE */
		context->rsvd_bits_mask[0][1] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 62);	/* PDE */
		context->rsvd_bits_mask[0][0] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 62); 	/* PTE */
		context->rsvd_bits_mask[1][1] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 62) |
			rsvd_bits(13, 20);		/* large page */
		context->rsvd_bits_mask[1][0] = context->rsvd_bits_mask[1][0];
		break;
	case PT64_ROOT_LEVEL:
		context->rsvd_bits_mask[0][3] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 51) | rsvd_bits(7, 8);
		context->rsvd_bits_mask[0][2] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 51) | rsvd_bits(7, 8);
		context->rsvd_bits_mask[0][1] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 51);
		context->rsvd_bits_mask[0][0] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 51);
		context->rsvd_bits_mask[1][3] = context->rsvd_bits_mask[0][3];
		context->rsvd_bits_mask[1][2] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 51) |
			rsvd_bits(13, 29);
		context->rsvd_bits_mask[1][1] = exb_bit_rsvd |
			rsvd_bits(maxphyaddr, 51) |
			rsvd_bits(13, 20);		/* large page */
		context->rsvd_bits_mask[1][0] = context->rsvd_bits_mask[1][0];
		break;
	}
}

caddr_t pfn_to_page(pfn_t pfn)
{
	/*
	 * XXX This routine takes a page frame number and
	 * returns a virtual address referring to the page.
	 */
	return (caddr_t)NULL;  /* XXX fix me!!! */
}

	
extern caddr_t gfn_to_page(struct kvm *kvm, gfn_t gfn);

#define PT64_LEVEL_BITS 9

#define PT64_LEVEL_SHIFT(level) \
		(PAGESHIFT + (level - 1) * PT64_LEVEL_BITS)

#define PT64_LEVEL_MASK(level) \
		(((1ULL << PT64_LEVEL_BITS) - 1) << PT64_LEVEL_SHIFT(level))

#define PT64_INDEX(address, level)\
	(((address) >> PT64_LEVEL_SHIFT(level)) & ((1 << PT64_LEVEL_BITS) - 1))

#define SHADOW_PT_INDEX(addr, level) PT64_INDEX(addr, level)

#define PT64_DIR_BASE_ADDR_MASK \
	(PT64_BASE_ADDR_MASK & ~((1ULL << (PAGESHIFT + PT64_LEVEL_BITS)) - 1))
#define PT64_LVL_ADDR_MASK(level) \
	(PT64_BASE_ADDR_MASK & ~((1ULL << (PAGESHIFT + (((level) - 1) \
						* PT64_LEVEL_BITS))) - 1))
#define PT64_LVL_OFFSET_MASK(level) \
	(PT64_BASE_ADDR_MASK & ((1ULL << (PAGESHIFT + (((level) - 1) \
						* PT64_LEVEL_BITS))) - 1))

#define PT32_LEVEL_BITS 10
#define PT32_BASE_ADDR_MASK PAGEMASK

#define PT32_LEVEL_SHIFT(level) \
		(PAGESHIFT + (level - 1) * PT32_LEVEL_BITS)

#define PT32_LEVEL_MASK(level) \
		(((1ULL << PT32_LEVEL_BITS) - 1) << PT32_LEVEL_SHIFT(level))
#define PT32_LVL_OFFSET_MASK(level) \
	(PT32_BASE_ADDR_MASK & ((1ULL << (PAGESHIFT + (((level) - 1) \
						* PT32_LEVEL_BITS))) - 1))

#define PT32_LVL_ADDR_MASK(level) \
	(PAGEMASK & ~((1ULL << (PAGESHIFT + (((level) - 1) \
					    * PT32_LEVEL_BITS))) - 1))

#define PT32_INDEX(address, level)\
	(((address) >> PT32_LEVEL_SHIFT(level)) & ((1 << PT32_LEVEL_BITS) - 1))

static void shadow_walk_init(struct kvm_shadow_walk_iterator *iterator,
			     struct kvm_vcpu *vcpu, uint64_t addr)
{
	iterator->addr = addr;
	iterator->shadow_addr = vcpu->arch.mmu.root_hpa;
	iterator->level = vcpu->arch.mmu.shadow_root_level;
	if (iterator->level == PT32E_ROOT_LEVEL) {
		iterator->shadow_addr
			= vcpu->arch.mmu.pae_root[(addr >> 30) & 3];
		iterator->shadow_addr &= PT64_BASE_ADDR_MASK;
		--iterator->level;
		if (!iterator->shadow_addr)
			iterator->level = 0;
	}
}

static int shadow_walk_okay(struct kvm_shadow_walk_iterator *iterator)
{
	if (iterator->level < PT_PAGE_TABLE_LEVEL)
		return 0;

	if (iterator->level == PT_PAGE_TABLE_LEVEL)
		if (is_large_pte(*iterator->sptep))
			return 0;

	iterator->index = SHADOW_PT_INDEX(iterator->addr, iterator->level);
	iterator->sptep	= ((uint64_t *)(iterator->shadow_addr)) + iterator->index;
	return 1;
}

static void shadow_walk_next(struct kvm_shadow_walk_iterator *iterator)
{
	iterator->shadow_addr = *iterator->sptep & PT64_BASE_ADDR_MASK;
	--iterator->level;
}

#define for_each_shadow_entry(_vcpu, _addr, _walker)    \
	for (shadow_walk_init(&(_walker), _vcpu, _addr);	\
	     shadow_walk_okay(&(_walker));			\
	     shadow_walk_next(&(_walker)))

int kvm_read_guest_atomic(struct kvm *kvm, gpa_t gpa, void *data,
			  unsigned long len)
{
	int r;
	unsigned long addr;
	gfn_t gfn = gpa >> PAGESHIFT;
	int offset = offset_in_page(gpa);

	addr = gfn_to_hva(kvm, gfn);
	if (kvm_is_error_hva(addr))
		return -EFAULT;
#ifdef XXX
	pagefault_disable();
#endif /*XXX*/
	r = copyin((caddr_t)addr + offset, data, len);
#ifdef XXX
	pagefault_enable();
#endif /*XXX*/
	if (r)
		return -EFAULT;
	return 0;
}

void kvm_flush_remote_tlbs(struct kvm *kvm)
{
#ifdef XXX
	if (make_all_cpus_request(kvm, KVM_REQ_TLB_FLUSH))
		++kvm->stat.remote_tlb_flush;
#endif /*XXX*/
}

inline uint64_t kvm_pdptr_read(struct kvm_vcpu *vcpu, int index)
{
	if (!BT_TEST((unsigned long *)&vcpu->arch.regs_avail,
		     VCPU_EXREG_PDPTR))
		kvm_x86_ops->cache_reg(vcpu, VCPU_EXREG_PDPTR);

	return vcpu->arch.pdptrs[index];
}

extern void kvm_inject_page_fault(struct kvm_vcpu *vcpu, unsigned long addr,
				  uint32_t error_code);

static void inject_page_fault(struct kvm_vcpu *vcpu,
			      uint64_t addr,
			      uint32_t err_code)
{
	kvm_inject_page_fault(vcpu, addr, err_code);
}

gfn_t unalias_gfn(struct kvm *kvm, gfn_t gfn)
{
	int i;
	struct kvm_mem_alias *alias;
	struct kvm_mem_aliases *aliases;

	/* XXX need protection */
	aliases = kvm->arch.aliases;

	for (i = 0; i < aliases->naliases; ++i) {
		alias = &aliases->aliases[i];
		if (gfn >= alias->base_gfn
		    && gfn < alias->base_gfn + alias->npages)
			return alias->target_gfn + gfn - alias->base_gfn;
	}
	return gfn;
}

static inline int is_pse(struct kvm_vcpu *vcpu)
{
	return kvm_read_cr4_bits(vcpu, X86_CR4_PSE);
}

static int is_rsvd_bits_set(struct kvm_vcpu *vcpu, uint64_t gpte, int level)
{
	int bit7;

	bit7 = (gpte >> 7) & 1;
	return (gpte & vcpu->arch.mmu.rsvd_bits_mask[bit7][level-1]) != 0;
}

extern inline int is_pae(struct kvm_vcpu *vcpu);

int is_dirty_gpte(unsigned long pte)
{
	return pte & PT_DIRTY_MASK;
}

gfn_t pse36_gfn_delta(uint32_t gpte)
{
	int shift = 32 - PT32_DIR_PSE36_SHIFT - PAGESHIFT;

	return (gpte & PT32_DIR_PSE36_MASK) << shift;
}

void kvm_get_pfn(pfn_t pfn)
{
#ifdef XXX
	if (!kvm_is_mmio_pfn(pfn))
		get_page(pfn_to_page(pfn));
#endif /*XXX*/
}

#define PTTYPE 64
#include "paging_tmpl.h"
#undef PTTYPE

#define PTTYPE 32
#include "paging_tmpl.h"
#undef PTTYPE

void mmu_pte_write_new_pte(struct kvm_vcpu *vcpu,
				  struct kvm_mmu_page *sp,
				  uint64_t *spte,
				  const void *new)
{
	if (sp->role.level != PT_PAGE_TABLE_LEVEL) {
#ifdef XXX
		++vcpu->kvm->stat.mmu_pde_zapped;
#endif /*XXX*/
		return;
        }

#ifdef XXX
	++vcpu->kvm->stat.mmu_pte_updated;
#endif /*XXX*/
	if (sp->role.glevels == PT32_ROOT_LEVEL)
		paging32_update_pte(vcpu, sp, spte, new);
	else
		paging64_update_pte(vcpu, sp, spte, new);
}


static int init_kvm_tdp_mmu(struct kvm_vcpu *vcpu)
{
	struct kvm_mmu *context = &vcpu->arch.mmu;

	context->new_cr3 = nonpaging_new_cr3;
	context->page_fault = tdp_page_fault;
	context->free = nonpaging_free;
	context->prefetch_page = nonpaging_prefetch_page;
	context->sync_page = nonpaging_sync_page;
	context->invlpg = nonpaging_invlpg;
	context->shadow_root_level = kvm_x86_ops->get_tdp_level();
	context->root_hpa = INVALID_PAGE;

	if (!is_paging(vcpu)) {
		context->gva_to_gpa = nonpaging_gva_to_gpa;
		context->root_level = 0;
	} else if (is_long_mode(vcpu)) {
		reset_rsvds_bits_mask(vcpu, PT64_ROOT_LEVEL);
		context->gva_to_gpa = paging64_gva_to_gpa;
		context->root_level = PT64_ROOT_LEVEL;
	} else if (is_pae(vcpu)) {
		reset_rsvds_bits_mask(vcpu, PT32E_ROOT_LEVEL);
		context->gva_to_gpa = paging64_gva_to_gpa;
		context->root_level = PT32E_ROOT_LEVEL;
	} else {
		reset_rsvds_bits_mask(vcpu, PT32_ROOT_LEVEL);
		context->gva_to_gpa = paging32_gva_to_gpa;
		context->root_level = PT32_ROOT_LEVEL;
	}

	return 0;
}

static int nonpaging_map(struct kvm_vcpu *vcpu, gva_t v, int write, gfn_t gfn)
{
	int r;
	int level;
	pfn_t pfn;
	unsigned long mmu_seq;

	level = mapping_level(vcpu, gfn);

	/*
	 * This path builds a PAE pagetable - so we can map 2mb pages at
	 * maximum. Therefore check if the level is larger than that.
	 */
	if (level > PT_DIRECTORY_LEVEL)
		level = PT_DIRECTORY_LEVEL;

	gfn &= ~(KVM_PAGES_PER_HPAGE(level) - 1);

#ifdef XXX
	mmu_seq = vcpu->kvm->mmu_notifier_seq;
	smp_rmb();
#endif
	pfn = gfn_to_pfn(vcpu->kvm, gfn);

	/* mmio */
	if (is_error_pfn(pfn)) {
		kvm_release_pfn_clean(pfn);
		return 1;
	}

	mutex_enter(&vcpu->kvm->mmu_lock);
#ifdef XXX
	if (mmu_notifier_retry(vcpu, mmu_seq))
		goto out_unlock;
#endif /*XXX*/
	kvm_mmu_free_some_pages(vcpu);
	r = __direct_map(vcpu, v, write, level, gfn, pfn);
	mutex_exit(&vcpu->kvm->mmu_lock);


	return r;

out_unlock:
	mutex_enter(&vcpu->kvm->mmu_lock);
	kvm_release_pfn_clean(pfn);
	return 0;
}

static int nonpaging_page_fault(struct kvm_vcpu *vcpu, gva_t gva,
				uint32_t error_code)
{
	gfn_t gfn;
	int r;

	cmn_err(CE_NOTE, "%s: gva %lx error %x\n", __func__, gva, error_code);
	r = mmu_topup_memory_caches(vcpu);
	if (r)
		return r;

	ASSERT(vcpu);
	ASSERT(VALID_PAGE(vcpu->arch.mmu.root_hpa));

	gfn = gva >> PAGESHIFT;

	return nonpaging_map(vcpu, gva & PAGEMASK,
			     error_code & PFERR_WRITE_MASK, gfn);
}

static int nonpaging_init_context(struct kvm_vcpu *vcpu)
{
	struct kvm_mmu *context = &vcpu->arch.mmu;

	context->new_cr3 = nonpaging_new_cr3;
	context->page_fault = nonpaging_page_fault;
	context->gva_to_gpa = nonpaging_gva_to_gpa;
	context->free = nonpaging_free;
	context->prefetch_page = nonpaging_prefetch_page;
	context->sync_page = nonpaging_sync_page;
	context->invlpg = nonpaging_invlpg;
	context->root_level = 0;
	context->shadow_root_level = PT32E_ROOT_LEVEL;
	context->root_hpa = INVALID_PAGE;
	return 0;
}

static int paging64_init_context_common(struct kvm_vcpu *vcpu, int level)
{
	struct kvm_mmu *context = &vcpu->arch.mmu;

	ASSERT(is_pae(vcpu));
	context->new_cr3 = paging_new_cr3;
	context->page_fault = paging64_page_fault;
	context->gva_to_gpa = paging64_gva_to_gpa;
	context->prefetch_page = paging64_prefetch_page;
	context->sync_page = paging64_sync_page;
	context->invlpg = paging64_invlpg;
	context->free = paging_free;
	context->root_level = level;
	context->shadow_root_level = level;
	context->root_hpa = INVALID_PAGE;
	return 0;
}

static int paging64_init_context(struct kvm_vcpu *vcpu)
{
	reset_rsvds_bits_mask(vcpu, PT64_ROOT_LEVEL);
	return paging64_init_context_common(vcpu, PT64_ROOT_LEVEL);
}

static int paging32_init_context(struct kvm_vcpu *vcpu)
{
	struct kvm_mmu *context = &vcpu->arch.mmu;

	reset_rsvds_bits_mask(vcpu, PT32_ROOT_LEVEL);
	context->new_cr3 = paging_new_cr3;
	context->page_fault = paging32_page_fault;
	context->gva_to_gpa = paging32_gva_to_gpa;
	context->free = paging_free;
	context->prefetch_page = paging32_prefetch_page;
	context->sync_page = paging32_sync_page;
	context->invlpg = paging32_invlpg;
	context->root_level = PT32_ROOT_LEVEL;
	context->shadow_root_level = PT32E_ROOT_LEVEL;
	context->root_hpa = INVALID_PAGE;
	return 0;
}

static int paging32E_init_context(struct kvm_vcpu *vcpu)
{
	reset_rsvds_bits_mask(vcpu, PT32E_ROOT_LEVEL);
	return paging64_init_context_common(vcpu, PT32E_ROOT_LEVEL);
}

static int init_kvm_softmmu(struct kvm_vcpu *vcpu)
{
	int r;

	ASSERT(vcpu);
	ASSERT(!VALID_PAGE(vcpu->arch.mmu.root_hpa));

	if (!is_paging(vcpu))
		r = nonpaging_init_context(vcpu);
	else if (is_long_mode(vcpu))
		r = paging64_init_context(vcpu);
	else if (is_pae(vcpu))
		r = paging32E_init_context(vcpu);
	else
		r = paging32_init_context(vcpu);

	vcpu->arch.mmu.base_role.glevels = vcpu->arch.mmu.root_level;

	return r;
}

int init_kvm_mmu(struct kvm_vcpu *vcpu)
{
	vcpu->arch.update_pte.pfn = -1; /* bad_pfn */

#ifdef XXX
	/*
	 * XXX currently, we won't support 2 dimensional paging.
	 * So the hardware will not do guest-virtual to guest-physical
	 * and guest-physical to host physical.  So we'll need to
	 * implement "shadow" paging...
	 */
  
	if (tdp_enabled)
		return init_kvm_tdp_mmu(vcpu);
	else
#endif
		return init_kvm_softmmu(vcpu);
	return 0;
}

int kvm_mmu_setup(struct kvm_vcpu *vcpu)
{
	ASSERT(vcpu);

	return init_kvm_mmu(vcpu);
}

int
kvm_arch_vcpu_setup(struct kvm_vcpu *vcpu)
{
	int r;

#ifdef XXX
	/* We do fxsave: this must be aligned. */
	BUG_ON((unsigned long)&vcpu->arch.host_fx_image & 0xF);
#endif

	vcpu->arch.mtrr_state.have_fixed = 1;
	vcpu_load(vcpu);
	r = kvm_arch_vcpu_reset(vcpu);
	if (r == 0)
		r = kvm_mmu_setup(vcpu);
	vcpu_put(vcpu);
	if (r < 0)
		goto free_vcpu;

	return 0;
free_vcpu:
#ifdef XXX
	kvm_x86_ops->vcpu_free(vcpu);
#endif
	return r;
}

void kvm_get_kvm(struct kvm *kvm)
{
	atomic_inc_32(&kvm->users_count);
}

/*
 * Creates some virtual cpus.  Good luck creating more than one.
 */
int
kvm_vm_ioctl_create_vcpu(struct kvm *kvm, int32_t id, struct kvm_vcpu_ioc *arg, int *rval_p)
{
	int r;
	struct kvm_vcpu *vcpu, *v;

	vcpu = kvm_arch_vcpu_create(kvm, arg, id);
	if (vcpu == NULL)
		return EINVAL;

#ifdef XXX
	preempt_notifier_init(&vcpu->preempt_notifier, &kvm_preempt_ops);
#endif

	r = kvm_arch_vcpu_setup(vcpu);
	if (r)
		return r;

	mutex_enter(&kvm->lock);
#ifdef XXX
	if (atomic_read(&kvm->online_vcpus) == KVM_MAX_VCPUS) {
		r = -EINVAL;
		goto vcpu_destroy;
	}

	kvm_for_each_vcpu(r, v, kvm)
		if (v->vcpu_id == id) {
			r = -EEXIST;
			goto vcpu_destroy;
		}

	BUG_ON(kvm->vcpus[atomic_read(&kvm->online_vcpus)]);

#endif /*XXX*/

	/* Now it's all set up, let userspace reach it */
	kvm_get_kvm(kvm);

	*rval_p = kvm->online_vcpus;  /* guarantee unique id */
	vcpu->vcpu_id = *rval_p;

	/* XXX need to protect online_vcpus */
	kvm->vcpus[kvm->online_vcpus++] = vcpu;

#ifdef XXX
	smp_wmb();
#endif /*XXX*/
	atomic_inc_32(&kvm->online_vcpus);

#ifdef CONFIG_KVM_APIC_ARCHITECTURE
	if (kvm->bsp_vcpu_id == id)
		kvm->bsp_vcpu = vcpu;
#endif

	mutex_exit(&kvm->lock);
	return r;

vcpu_destroy:
#ifdef NOTNOW
	mutex_exit(&kvm->lock);
	kvm_arch_vcpu_destroy(vcpu);
#endif /*NOTNOW*/
	return r;
}

extern int largepages_enabled;

extern caddr_t smmap32(caddr32_t addr, size32_t len, int prot, int flags, int fd, off32_t pos);

int kvm_arch_prepare_memory_region(struct kvm *kvm,
				struct kvm_memory_slot *memslot,
				struct kvm_memory_slot old,
				struct kvm_userspace_memory_region *mem,
				int user_alloc)
{
	int npages = memslot->npages;

	/*To keep backward compatibility with older userspace,
	 *x86 needs to hanlde !user_alloc case.
	 */
	if (!user_alloc) {
		if (npages && !old.rmap) {
#ifdef XXX
			unsigned long userspace_addr;
			down_write(&current->mm->mmap_sem);
			userspace_addr = do_mmap(NULL, 0,
						 npages * PAGESIZE,
						 PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS,
						 0);
			up_write(&current->mm->mmap_sem);

			if (IS_ERR((void *)userspace_addr))
				return PTR_ERR((void *)userspace_addr);
			memslot->userspace_addr = (unsigned long) userspace_addr;
#else
			{
				int rval;
				caddr_t userspace_addr = NULL;
				userspace_addr = smmap32(NULL, npages*PAGESIZE,
							 PROT_READ|PROT_WRITE,
							 MAP_PRIVATE|MAP_ANON,
							 -1, 0);
				cmn_err(CE_NOTE, "kvm_arch_prepare_memory_region: mmap at %lx\n", userspace_addr);
				memslot->userspace_addr = (unsigned long) userspace_addr;
			}
#endif /*XXX*/

		}
	}

	return 0;
}

/*
 * Allocate some memory and give it an address in the guest physical address
 * space.
 *
 * Discontiguous memory is allowed, mostly for framebuffers.
 *
 * Must be called holding mmap_sem for write.
 */

extern void kvm_arch_commit_memory_region(struct kvm *kvm,
				struct kvm_userspace_memory_region *mem,
				struct kvm_memory_slot old,
					  int user_alloc);

extern int __kvm_set_memory_region(struct kvm *kvm,
			    struct kvm_userspace_memory_region *mem,
				   int user_alloc);

extern int kvm_set_memory_region(struct kvm *kvm,
			  struct kvm_userspace_memory_region *mem,
				 int user_alloc);

int kvm_vm_ioctl_set_memory_region(struct kvm *kvm,
				   struct
				   kvm_userspace_memory_region *mem,
				   int user_alloc)
{
	if (mem->slot >= KVM_MEMORY_SLOTS)
		return EINVAL;
	return kvm_set_memory_region(kvm, mem, user_alloc);
}

static inline struct kvm_coalesced_mmio_dev *to_mmio(struct kvm_io_device *dev)
{
	return container_of(dev, struct kvm_coalesced_mmio_dev, dev);
}

static int coalesced_mmio_in_range(struct kvm_coalesced_mmio_dev *dev,
				   gpa_t addr, int len)
{
	struct kvm_coalesced_mmio_zone *zone;
	struct kvm_coalesced_mmio_ring *ring;
	unsigned avail;
	int i;

	/* Are we able to batch it ? */

	/* last is the first free entry
	 * check if we don't meet the first used entry
	 * there is always one unused entry in the buffer
	 */
	ring = dev->kvm->coalesced_mmio_ring;
	avail = (ring->first - ring->last - 1) % KVM_COALESCED_MMIO_MAX;
	if (avail < KVM_MAX_VCPUS) {
		/* full */
		return 0;
	}

	/* is it in a batchable area ? */

	for (i = 0; i < dev->nb_zones; i++) {
		zone = &dev->zone[i];

		/* (addr,len) is fully included in
		 * (zone->addr, zone->size)
		 */

		if (zone->addr <= addr &&
		    addr + len <= zone->addr + zone->size)
			return 1;
	}
	return 0;
}

/* Caller must hold slots_lock. */
int kvm_io_bus_register_dev(struct kvm *kvm, enum kvm_bus bus_idx,
			    struct kvm_io_device *dev)
{
	struct kvm_io_bus *new_bus, *bus;

	bus = kvm->buses[bus_idx];
	if (bus->dev_count > NR_IOBUS_DEVS-1)
		return -ENOSPC;

	new_bus = kmem_zalloc(sizeof(struct kvm_io_bus), KM_SLEEP);
	if (!new_bus)
		return -ENOMEM;
	memcpy(new_bus, bus, sizeof(struct kvm_io_bus));
	new_bus->devs[new_bus->dev_count++] = dev;
#ifdef XXX
	rcu_assign_pointer(kvm->buses[bus_idx], new_bus);
	synchronize_srcu_expedited(&kvm->srcu);
#endif /*XXX*/
	kmem_free(bus, sizeof(struct kvm_io_bus));

	return 0;
}

/* Caller must hold slots_lock. */
int kvm_io_bus_unregister_dev(struct kvm *kvm, enum kvm_bus bus_idx,
			      struct kvm_io_device *dev)
{
	int i, r;
	struct kvm_io_bus *new_bus, *bus;

	new_bus = kmem_zalloc(sizeof(struct kvm_io_bus), KM_SLEEP);
	if (!new_bus)
		return -ENOMEM;

	bus = kvm->buses[bus_idx];
	memcpy(new_bus, bus, sizeof(struct kvm_io_bus));

	r = -ENOENT;
	for (i = 0; i < new_bus->dev_count; i++)
		if (new_bus->devs[i] == dev) {
			r = 0;
			new_bus->devs[i] = new_bus->devs[--new_bus->dev_count];
			break;
		}

	if (r) {
		kmem_free(new_bus, sizeof(struct kvm_io_bus));
		return r;
	}

#ifdef XXX
	rcu_assign_pointer(kvm->buses[bus_idx], new_bus);
	synchronize_srcu_expedited(&kvm->srcu);
#endif
	kmem_free(bus, sizeof(struct kvm_io_bus));
	return r;
}

static int coalesced_mmio_write(struct kvm_io_device *this,
				gpa_t addr, int len, const void *val);
static void coalesced_mmio_destructor(struct kvm_io_device *this);

static const struct kvm_io_device_ops coalesced_mmio_ops = {
	.write      = coalesced_mmio_write,
	.destructor = coalesced_mmio_destructor,
};

static int coalesced_mmio_write(struct kvm_io_device *this,
				gpa_t addr, int len, const void *val)
{
	struct kvm_coalesced_mmio_dev *dev = to_mmio(this);
	struct kvm_coalesced_mmio_ring *ring = dev->kvm->coalesced_mmio_ring;
	if (!coalesced_mmio_in_range(dev, addr, len))
		return -EOPNOTSUPP;

	mutex_enter(&dev->lock);

	/* copy data in first free entry of the ring */

	ring->coalesced_mmio[ring->last].phys_addr = addr;
	ring->coalesced_mmio[ring->last].len = len;
	memcpy(ring->coalesced_mmio[ring->last].data, val, len);
#ifdef XXX
	smp_wmb();
#endif /*XXX*/
	ring->last = (ring->last + 1) % KVM_COALESCED_MMIO_MAX;
	mutex_exit(&dev->lock);
	return 0;
}

static void coalesced_mmio_destructor(struct kvm_io_device *this)
{
	struct kvm_coalesced_mmio_dev *dev = to_mmio(this);

	kmem_free(dev, sizeof(struct kvm_coalesced_mmio_dev));
}

int kvm_coalesced_mmio_init(struct kvm *kvm)
{
	struct kvm_coalesced_mmio_dev *dev;
	caddr_t *page;
	int ret;

	ret = -ENOMEM;
	page = kmem_zalloc(PAGESIZE, KM_SLEEP);
	if (!page)
		goto out_err;
	kvm->coalesced_mmio_ring = (struct kvm_coalesced_mmio_ring *)page;

	ret = -ENOMEM;
	dev = kmem_alloc(sizeof(struct kvm_coalesced_mmio_dev), KM_SLEEP);
	if (!dev)
		goto out_free_page;
	mutex_init(&dev->lock, NULL, MUTEX_DRIVER, 0);
	kvm_iodevice_init(&dev->dev, &coalesced_mmio_ops);
	dev->kvm = kvm;
	kvm->coalesced_mmio_dev = dev;

	mutex_enter(&kvm->slots_lock);
	ret = kvm_io_bus_register_dev(kvm, KVM_MMIO_BUS, &dev->dev);
	mutex_exit(&kvm->slots_lock);
	if (ret < 0)
		goto out_free_dev;

	return ret;

out_free_dev:
	kmem_free(dev, sizeof(struct kvm_coalesced_mmio_dev));
out_free_page:
	kmem_free(page, PAGESIZE);
out_err:
	return ret;
}

void kvm_coalesced_mmio_free(struct kvm *kvm)
{
	if (kvm->coalesced_mmio_ring)
		kmem_free(kvm->coalesced_mmio_ring, PAGESIZE);
}

int kvm_vm_ioctl_register_coalesced_mmio(struct kvm *kvm,
					 struct kvm_coalesced_mmio_zone *zone)
{
	struct kvm_coalesced_mmio_dev *dev = kvm->coalesced_mmio_dev;

	if (dev == NULL)
		return -EINVAL;

	mutex_enter(&kvm->slots_lock);
	if (dev->nb_zones >= KVM_COALESCED_MMIO_ZONE_MAX) {
		mutex_exit(&kvm->slots_lock);
		return -ENOBUFS;
	}

	dev->zone[dev->nb_zones] = *zone;
	dev->nb_zones++;

	mutex_exit(&kvm->slots_lock);
	return 0;
}

int kvm_vm_ioctl_unregister_coalesced_mmio(struct kvm *kvm,
					   struct kvm_coalesced_mmio_zone *zone)
{
	int i;
	struct kvm_coalesced_mmio_dev *dev = kvm->coalesced_mmio_dev;
	struct kvm_coalesced_mmio_zone *z;

	if (dev == NULL)
		return -EINVAL;

	mutex_enter(&kvm->slots_lock);

	i = dev->nb_zones;
	while (i) {
		z = &dev->zone[i - 1];

		/* unregister all zones
		 * included in (zone->addr, zone->size)
		 */

		if (zone->addr <= z->addr &&
		    z->addr + z->size <= zone->addr + zone->size) {
			dev->nb_zones--;
			*z = dev->zone[dev->nb_zones];
		}
		i--;
	}

	mutex_exit(&kvm->slots_lock);

	return 0;
}

long
kvm_vm_ioctl(struct kvm *kvmp, unsigned int ioctl, unsigned long arg, int mode)
{
	void *argp = (void  *)arg;
	int r;
	proc_t *p;

	if (drv_getparm(UPROCP, &p) != 0)
		cmn_err(CE_PANIC, "Cannot get proc_t for current process\n");

	cmn_err(CE_NOTE, "kvm_vm_ioctl: cmd = %x\n", ioctl);
	cmn_err(CE_CONT, "kvm_vm_ioctl: KVM_SET_USER_MEMORY_REGION = %x\n",
		KVM_SET_USER_MEMORY_REGION);
	if (kvmp->mm != p->p_as)
		return EIO;
	switch (ioctl) {
#ifdef NOTNOW
	case KVM_GET_DIRTY_LOG: {
		struct kvm_dirty_log log;

		r = -EFAULT;
		if (copy_from_user(&log, argp, sizeof log))
			goto out;
		r = kvm_vm_ioctl_get_dirty_log(kvmp, &log);
		if (r)
			goto out;
		break;
	}
#endif /*NOTNOW*/

#ifdef KVM_COALESCED_MMIO_PAGE_OFFSET
	case KVM_REGISTER_COALESCED_MMIO: {
		struct kvm_coalesced_mmio_zone zone;
		r = EFAULT;
		if (copyin(argp, &zone, sizeof zone))
			goto out;
		r = ENXIO;
		r = kvm_vm_ioctl_register_coalesced_mmio(kvmp, &zone);
		if (r)
			goto out;
		r = 0;
		break;
	}
	case KVM_UNREGISTER_COALESCED_MMIO: {
		struct kvm_coalesced_mmio_zone zone;
		r = EFAULT;
		if (copyin(argp, &zone, sizeof zone))
			goto out;
		r = ENXIO;
		r = kvm_vm_ioctl_unregister_coalesced_mmio(kvmp, &zone);
		if (r)
			goto out;
		r = 0;
		break;
	}
#endif
#ifdef XXX
	case KVM_IRQFD: {
		struct kvm_irqfd data;

		if (ddi_copyin(argp, &data, sizeof data, mode))
			return (EFAULT);
		r = kvm_irqfd(kvmp, data.fd, data.gsi, data.flags);
		break;
	}
	case KVM_IOEVENTFD: {
		struct kvm_ioeventfd data;

		r = -EFAULT;
		if (copy_from_user(&data, argp, sizeof data))
			goto out;
		r = kvm_ioeventfd(kvmp, &data);
		break;
	}

#ifdef CONFIG_KVM_APIC_ARCHITECTURE
	case KVM_SET_BOOT_CPU_ID:
		r = 0;
		mutex_enter(&kvmp->lock);
		if (atomic_read(&kvmp->online_vcpus) != 0)
			r = -EBUSY;
		else
			kvmp->bsp_vcpu_id = arg;
		mutex_exit(&kvmp->lock);
		break;
#endif
#endif /*XXX*/
	default:
		return EINVAL;
	}

out:
	return r;
}