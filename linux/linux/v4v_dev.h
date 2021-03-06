#ifndef __V4V_DGRAM_H__
#define __V4V_DGRAM_H__

#include <xen/v4v.h>

typedef enum
{
  V4V_PTYPE_DGRAM = 1,
  V4V_PTYPE_STREAM,
} v4v_ptype;

struct v4v_dev {
	void *buf;
	size_t len;
	int flags;
	v4v_addr_t *addr;
};

struct v4v_viptables_rule_pos {
	struct v4v_viptables_rule* rule;
	int position;
};

#define V4V_TYPE 'W'

#define V4VIOCSETRINGSIZE 	_IOW (V4V_TYPE,  1, uint32_t)
#define V4VIOCBIND		_IOW (V4V_TYPE,  2, struct v4v_ring_id)
#define V4VIOCGETSOCKNAME	_IOW (V4V_TYPE,  3, struct v4v_ring_id)
#define V4VIOCGETPEERNAME	_IOW (V4V_TYPE,  4, v4v_addr_t)
#define V4VIOCCONNECT		_IOW (V4V_TYPE,  5, v4v_addr_t)
#define V4VIOCGETCONNECTERR	_IOW (V4V_TYPE,  6, int)
#define V4VIOCLISTEN		_IOW (V4V_TYPE,  7, uint32_t) /*unused args */
#define V4VIOCACCEPT		_IOW (V4V_TYPE,  8, v4v_addr_t) 
#define V4VIOCSEND		_IOW (V4V_TYPE,  9, struct v4v_dev)
#define V4VIOCRECV		_IOW (V4V_TYPE, 10, struct v4v_dev)
#define V4VIOCVIPTABLESADD	_IOW (V4V_TYPE, 11, struct v4v_viptables_rule_pos)
#define V4VIOCVIPTABLESDEL	_IOW (V4V_TYPE, 12, struct v4v_viptables_rule_pos)
#define V4VIOCVIPTABLESLIST	_IOW (V4V_TYPE, 13, uint32_t) /*unused args */
#define V4VIOCGETSOCKTYPE	_IOW (V4V_TYPE, 14, int)

#endif
