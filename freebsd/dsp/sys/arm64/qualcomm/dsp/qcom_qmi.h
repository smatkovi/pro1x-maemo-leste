/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QMI, Qualcomm's messages in TLVs over QRTR (Linux's
 * include/linux/soc/qcom/qmi.h, with its names and values): the encoder
 * (qmi_encdec.c) and the client and server side (qmi_core.c, over this
 * kernel's QRTR ports). Linux drivers built with LinuxKPI (ath10k's qmi.c)
 * get these declarations through linux/soc/qcom/qmi.h unchanged; the
 * structures they embed (qmi_handle, qmi_txn) keep this module's state
 * behind a pointer, so their layout is the same on both sides.
 */

#ifndef _QCOM_QMI_H_
#define	_QCOM_QMI_H_

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#endif

struct sockaddr_qrtr;

struct qmi_header {
	uint8_t		type;
	uint16_t	txn_id;
	uint16_t	msg_id;
	uint16_t	msg_len;
} __attribute__((__packed__));

#define	QMI_REQUEST	0
#define	QMI_RESPONSE	2
#define	QMI_INDICATION	4

#define	QMI_COMMON_TLV_TYPE 0

enum qmi_elem_type {
	QMI_EOTI,
	QMI_OPT_FLAG,
	QMI_DATA_LEN,
	QMI_UNSIGNED_1_BYTE,
	QMI_UNSIGNED_2_BYTE,
	QMI_UNSIGNED_4_BYTE,
	QMI_UNSIGNED_8_BYTE,
	QMI_SIGNED_2_BYTE_ENUM,
	QMI_SIGNED_4_BYTE_ENUM,
	QMI_STRUCT,
	QMI_STRING,
};

enum qmi_array_type {
	NO_ARRAY,
	STATIC_ARRAY,
	VAR_LEN_ARRAY,
};

/* One element of a message: its type, size, TLV and place in the C structure */
struct qmi_elem_info {
	enum qmi_elem_type		data_type;
	uint32_t			elem_len;
	uint32_t			elem_size;
	enum qmi_array_type		array_type;
	uint8_t				tlv_type;
	uint32_t			offset;
	const struct qmi_elem_info	*ei_array;
};

#define	QMI_RESULT_SUCCESS_V01			0
#define	QMI_RESULT_FAILURE_V01			1

#define	QMI_ERR_NONE_V01			0
#define	QMI_ERR_MALFORMED_MSG_V01		1
#define	QMI_ERR_NO_MEMORY_V01			2
#define	QMI_ERR_INTERNAL_V01			3
#define	QMI_ERR_CLIENT_IDS_EXHAUSTED_V01	5
#define	QMI_ERR_INVALID_ID_V01			41
#define	QMI_ERR_ENCODING_V01			58
#define	QMI_ERR_DISABLED_V01			69
#define	QMI_ERR_INCOMPATIBLE_STATE_V01		90
#define	QMI_ERR_NOT_SUPPORTED_V01		94

#define	QMI_SERVICE_ID_TEST		0x0f
#define	QMI_SERVICE_ID_SSCTL		0x2b
#define	QMI_SERVICE_ID_IPA		0x31
#define	QMI_SERVICE_ID_SERVREG_LOC	0x40
#define	QMI_SERVICE_ID_SERVREG_NOTIF	0x42
#define	QMI_SERVICE_ID_WLFW		0x45
#define	QMI_SERVICE_ID_SLIMBUS		0x301
#define	QMI_SERVICE_ID_USB_AUDIO_STREAM	0x41d

struct qmi_response_type_v01 {
	uint16_t	result;
	uint16_t	error;
};

extern const struct qmi_elem_info qmi_response_type_v01_ei[];

/* A server the name service told of (new_server, del_server) */
struct qmi_service {
	unsigned int	service;
	unsigned int	version;
	unsigned int	instance;
	unsigned int	node;
	unsigned int	port;
	void		*priv;
	struct qmi_service *next;	/* qmi_core.c's list */
};

struct qmi_handle;

struct qmi_ops {
	int	(*new_server)(struct qmi_handle *, struct qmi_service *);
	void	(*del_server)(struct qmi_handle *, struct qmi_service *);
	void	(*net_reset)(struct qmi_handle *);
	void	(*msg_handler)(struct qmi_handle *, struct sockaddr_qrtr *,
		    const void *, size_t);
	void	(*bye)(struct qmi_handle *, unsigned int node);
	void	(*del_client)(struct qmi_handle *, unsigned int node,
		    unsigned int port);
};

/* A request waiting for its response */
struct qmi_txn {
	struct qmi_handle		*qmi;
	uint16_t			id;
	int				result;
	const struct qmi_elem_info	*ei;
	void				*dest;
	void				*priv;	/* qmi_core.c */
};

/* What to do with an incoming request or indication of one message id */
struct qmi_msg_handler {
	unsigned int			type;
	unsigned int			msg_id;
	const struct qmi_elem_info	*ei;
	size_t				decoded_size;
	void	(*fn)(struct qmi_handle *, struct sockaddr_qrtr *,
		    struct qmi_txn *, const void *);
};

struct qmi_handle {
	void				*priv;	/* qmi_core.c */
	struct qmi_ops			ops;
	const struct qmi_msg_handler	*handlers;
	size_t				recv_buf_size;
	/*
	 * Linux's socket of the handle, which a client connects to its
	 * server (ath10k: kernel_connect(qmi->sock, ...)) to send requests
	 * without an address; here the handle itself (qmi_connect)
	 */
	struct qmi_handle		*sock;
};

int	qmi_handle_init(struct qmi_handle *qmi, size_t recv_buf_size,
	    const struct qmi_ops *ops, const struct qmi_msg_handler *handlers);
void	qmi_handle_release(struct qmi_handle *qmi);

/* requests and indications with no address (sq NULL) go to sq from now */
int	qmi_connect(struct qmi_handle *qmi, const struct sockaddr_qrtr *sq,
	    size_t len);

int	qmi_add_lookup(struct qmi_handle *qmi, unsigned int service,
	    unsigned int version, unsigned int instance);
int	qmi_add_server(struct qmi_handle *qmi, unsigned int service,
	    unsigned int version, unsigned int instance);

/*
 * timeout in ticks (jiffies). As Linux: qmi_txn_wait returns the
 * response's decoded length (qmi_decode_message, >= 0) or a negative
 * errno; qmi_send_* return 0 or a negative errno.
 */
int	qmi_txn_init(struct qmi_handle *qmi, struct qmi_txn *txn,
	    const struct qmi_elem_info *ei, void *c_struct);
int	qmi_txn_wait(struct qmi_txn *txn, unsigned long timeout);
void	qmi_txn_cancel(struct qmi_txn *txn);

ssize_t	qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
	    struct qmi_txn *txn, int msg_id, size_t len,
	    const struct qmi_elem_info *ei, const void *c_struct);
ssize_t	qmi_send_response(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
	    struct qmi_txn *txn, int msg_id, size_t len,
	    const struct qmi_elem_info *ei, const void *c_struct);
ssize_t	qmi_send_indication(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
	    int msg_id, size_t len, const struct qmi_elem_info *ei,
	    const void *c_struct);

/*
 * The encoder: a header and the TLVs of c_struct, in a buffer of
 * glink_zalloc (glink_free it); *len is the size the TLVs may take and
 * then the message's. NULL on error (*len is then the negative errno).
 */
void	*qmi_encode_message(int type, unsigned int msg_id, size_t *len,
	    unsigned int txn_id, const struct qmi_elem_info *ei,
	    const void *c_struct);
int	qmi_decode_message(const void *buf, size_t len,
	    const struct qmi_elem_info *ei, void *c_struct);

#endif /* _QCOM_QMI_H_ */
