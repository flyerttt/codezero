/*
 * Copyright (C) 2008 Bahadir Balban
 */
#ifndef __FS0_TASK_H__
#define __FS0_TASK_H__

#include <lib/idpool.h>
#include <l4/lib/list.h>
#include <l4/api/kip.h>

#define __TASKNAME__			__VFSNAME__

#define TASK_OFILES_MAX			32

/* Thread control block, fs0 portion */
struct tcb {
	l4id_t tid;
	struct list_head list;
	int fd[TASK_OFILES_MAX];
	struct id_pool *fdpool;
};

struct tcb *find_task(int tid);
int init_task_data(void);

#endif /* __FS0_TASK_H__ */