/* Copyright (c) 2019-2022 Homa Developers
 * SPDX-License-Identifier: BSD-1-Clause
 */

/* Functions for mocking that are exported to test code. */

extern int         mock_alloc_page_errors;
extern int         mock_alloc_skb_errors;
extern int         mock_bpage_size;
extern int         mock_bpage_shift;
extern int         mock_compound_order_mask;
extern int         mock_copy_data_errors;
extern int         mock_copy_to_user_dont_copy;
extern int         mock_copy_to_user_errors;
extern int         mock_cpu_idle;
extern cycles_t    mock_cycles;
extern int         mock_import_iovec_errors;
extern int         mock_import_ubuf_errors;
extern int         mock_ip6_xmit_errors;
extern int         mock_ip_queue_xmit_errors;
extern bool        mock_ipv6;
extern bool        mock_ipv6_default;
extern int         mock_kmalloc_errors;
extern int         mock_kthread_create_errors;
extern int         mock_register_protosw_errors;
extern char        mock_xmit_prios[];
extern int         mock_log_rcu_sched;
extern int         mock_max_grants;
extern int         mock_max_skb_frags;
extern int         mock_mtu;
extern struct net_device
		   mock_net_device;
extern __u64       mock_ns;
extern __u64       mock_ns_tick;
extern int         mock_numa_mask;
extern int         mock_page_nid_mask;
extern char        mock_printk_output[];
extern int         mock_route_errors;
extern int         mock_spin_lock_held;
extern struct task_struct
		   mock_task;
extern int         mock_trylock_errors;
extern int         mock_vmalloc_errors;
extern int         mock_xmit_log_verbose;
extern int         mock_xmit_log_homa_info;

struct page *
		   mock_alloc_pages(gfp_t gfp, unsigned order);
int         mock_check_error(int *errorMask);
void        mock_clear_xmit_prios(void);
void        mock_data_ready(struct sock *sk);
cycles_t    mock_get_cycles(void);
unsigned int
	    mock_get_mtu(const struct dst_entry *dst);
void        mock_get_page(struct page *page);
int         mock_page_refs(struct page *page);
int         mock_page_refs(struct page *page);
void        mock_put_page(struct page *page);
void        mock_rcu_read_lock(void);
void        mock_rcu_read_unlock(void);
struct ctl_table_header *
	    mock_register_net_sysctl(struct net *net,
				     const char *path,
				     struct ctl_table *table);
void        mock_set_core(int num);
void        mock_set_ipv6(struct homa_sock *hsk);
void        mock_spin_lock(spinlock_t *lock);
void        mock_spin_unlock(spinlock_t *lock);
int         mock_skb_count(void);
struct sk_buff *
            mock_skb_new(struct in6_addr *saddr, struct homa_common_hdr *h,
			int extra_bytes, int first_value);
void        mock_sock_destroy(struct homa_sock *hsk,
			      struct homa_socktab *socktab);
void        mock_sock_init(struct homa_sock *hsk, struct homa *homa,
			   int port);
void        mock_teardown(void);
void       *mock_vmalloc(size_t size);
