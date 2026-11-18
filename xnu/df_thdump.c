
/* fix-xcode16 P6: dump every thread of a task (user pc/lr + frame-pointer chain), appended to sleh.c.
 * Used for backboardd: shows where its sleeping threads wait. */
extern kern_return_t vm_map_read_user(vm_map_t map, vm_map_address_t src_addr, void *dst_p, vm_size_t size);

void df_task_threads_dump(void *tp);
void
df_task_threads_dump(void *tp)
{
	task_t t = (task_t)tp;
	thread_t th;
	int i = 0;
	vm_map_t map = get_task_map(t);
	task_lock(t);
	queue_iterate(&t->threads, th, thread_t, task_threads) {
		arm_saved_state_t *ss = th->machine.upcb;
		if (!ss || i >= 24) {
			continue;
		}
		uint64_t fp = saved_state64(ss)->fp;
		printf("BBTH %d pc 0x%llx lr 0x%llx\n", i, (unsigned long long)saved_state64(ss)->pc,
		    (unsigned long long)saved_state64(ss)->lr);
		for (int f = 0; f < 14 && fp; f++) {
			uint64_t fr[2] = {0, 0};
			if (vm_map_read_user(map, (vm_map_address_t)fp, fr, sizeof(fr)) != KERN_SUCCESS) {
				break;
			}
			printf("BBTH %d  ret 0x%llx\n", i, (unsigned long long)fr[1]);
			if (fr[0] <= fp) {
				break;
			}
			fp = fr[0];
		}
		i++;
	}
	task_unlock(t);
}
