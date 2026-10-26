#ifndef _MACH_TYPES_H_
#define _MACH_TYPES_H_

#include <stdint.h>
#include <stddef.h>

/* Mach basic types */
typedef uint32_t mach_port_t;
typedef uint32_t mach_port_name_t;
typedef uint32_t mach_port_right_t;
typedef int32_t  kern_return_t;

#define KERN_SUCCESS            0
#define KERN_INVALID_ADDRESS    1
#define KERN_PROTECTION_FAILURE 2
#define KERN_NO_SPACE           3
#define KERN_INVALID_ARGUMENT   4
#define KERN_FAILURE            5
#define KERN_RESOURCE_SHORTAGE  6
#define KERN_NOT_RECEIVER       7
#define KERN_NO_ACCESS          8

#define MACH_PORT_NULL          ((mach_port_t) 0)
#define MACH_PORT_DEAD          ((mach_port_t) ~0)

/* Mach message header structure */
typedef struct {
    uint32_t            msgh_bits;
    uint32_t            msgh_size;
    mach_port_t         msgh_remote_port;
    mach_port_t         msgh_local_port;
    uint32_t            msgh_voucher_port;
    int32_t             msgh_id;
} mach_msg_header_t;

/* Trap numbers (ARM64 Darwin conventions) */
#define MACH_TRAP_MACH_MSG          -31
#define MACH_TRAP_TASK_SELF         -28
#define MACH_TRAP_HOST_SELF         -29
#define MACH_TRAP_SEMAPHORE_WAIT    -36
#define MACH_TRAP_SEMAPHORE_SIGNAL  -37

#endif /* _MACH_TYPES_H_ */
