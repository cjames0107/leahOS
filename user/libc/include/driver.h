#ifndef _DRIVER_H
#define _DRIVER_H

#include <stdint.h>

/* What a driver outside the kernel is allowed to do.
 *
 * Not a ring. On x86-64 the page tables carry one privilege bit, so anything
 * below ring 3 is supervisor and can read every byte of kernel memory - a ring
 * number would have named the privilege without enforcing it. These four are
 * enforced: the first by the processor's I/O permission bitmap, the rest by
 * the page tables.
 *
 * Each is asked for separately and covers exactly what it names, so a sound
 * driver that asked for a mixer's ports cannot reach a disk controller's.
 */

/* Let this process use `count` I/O ports starting at `base`. 0 or -1. */
int io_permit(unsigned base, unsigned count);

/* Map a device's registers. Returns a pointer, or 0. The mapping is uncached:
 * a status register read out of a stale cache line is a bug that looks like
 * broken hardware. */
volatile void* map_physical(uint64_t physical, unsigned long bytes);

/* Physically contiguous memory that a device can be pointed at. Writes the
 * physical address through `physical_out`, because that is what goes in a
 * descriptor - there is no IOMMU here to translate on the driver's behalf. */
void* dma_alloc(unsigned long bytes, uint64_t* physical_out);

/* Claim an interrupt line, then block until it fires. irq_wait returns how many
 * times the line fired since the last call, so a driver that was slow finds out
 * it missed some rather than quietly handling one event for several. */
int  irq_listen(unsigned irq);
long irq_wait(unsigned irq);

/* Port I/O. Only legal once io_permit has covered the port; otherwise the
 * processor faults, which is the point. */
static inline void outb(unsigned short port, unsigned char v)
{ __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned char inb(unsigned short port)
{ unsigned char v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw(unsigned short port, unsigned short v)
{ __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned short inw(unsigned short port)
{ unsigned short v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outl(unsigned short port, unsigned int v)
{ __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned int inl(unsigned short port)
{ unsigned int v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

#endif
