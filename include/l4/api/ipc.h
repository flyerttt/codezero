#ifndef __IPC_H__
#define __IPC_H__


#define L4_NILTHREAD		-1
#define L4_ANYTHREAD		-2

#define L4_IPC_TAG_MR_OFFSET		0

/* Pagefault */
#define L4_IPC_TAG_PFAULT		0

#if defined (__KERNEL__)

/* These are for internally created ipc paths. */
int ipc_send(l4id_t to);
int ipc_sendrecv(l4id_t to, l4id_t from);

#endif

#endif /* __IPC_H__ */