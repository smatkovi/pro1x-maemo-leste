/*
 * Host test of qcom_cci.c's protocol against a model of the CCI: the queue
 * words it loads (Linux's layout: SET_PARAM with the 7-bit address, WRITE
 * with the length and data bytes packed little endian behind the command
 * byte, REPORT with its interrupt bit, READ with the length), the read
 * buffer (len / 4 + 1 words, the first byte not data), the status bits,
 * NACKs (halt, master reset), limits. The model's slave devices are
 * sensors with 16-bit register addresses that auto-increment.
 */
#define	CCI_HOST_TEST
#include "../sys/arm64/qualcomm/camss/qcom_cci.c"

#include <assert.h>
#include <stdlib.h>

static int fails;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
	__LINE__, #c); fails++; } } while (0)

/* --- the model ----------------------------------------------------------- */
static struct {
	uint32_t words[2][2][64];
	u_int nwords[2][2];
	uint32_t status;
	uint32_t rdbuf[2][8];
	u_int rdlevel[2], rdpos[2];
	uint32_t loaded_log[64];
	u_int nlog;
	int halted[2];
} m;

static struct slave {
	uint8_t addr;
	uint8_t master;
	uint16_t ptr;
	uint8_t regs[65536];
} slaves[2];

static struct slave *
find(u_int master, uint8_t addr)
{
	for (u_int i = 0; i < nitems(slaves); i++)
		if (slaves[i].addr == addr && slaves[i].master == master)
			return (&slaves[i]);
	return (NULL);
}

static void
run(u_int mm, u_int q)
{
	uint8_t addr = 0, bytes[64];
	u_int i = 0, n = m.nwords[mm][q], len, b, j;
	struct slave *s;

	while (i < n) {
		uint32_t w = m.words[mm][q][i];
		switch (w & 0xf) {
		case CCI_I2C_SET_PARAM:
			addr = (w >> 4) & 0x7f;
			i++;
			break;
		case CCI_I2C_WRITE:
			len = (w >> 4) & 0xf;
			/* bytes 1..3 of this word, then the following words */
			for (b = 0, j = 1; b < len; b++, j++)
				bytes[b] = m.words[mm][q][i + j / 4] >> (8 * (j % 4));
			i += (1 + len + 3) / 4;
			if ((s = find(mm, addr)) == NULL) {
				m.status |= IRQ_NACK(mm) & (q ? (1u << (28 + 2 * mm)) :
				    (1u << (27 + 2 * mm)));
				m.nwords[mm][q] = 0;
				return;
			}
			s->ptr = bytes[0] << 8 | bytes[1];
			for (b = 2; b < len; b++)
				s->regs[s->ptr++] = bytes[b];
			break;
		case CCI_I2C_READ:
			len = (w >> 4) & 0xf;
			i++;
			if ((s = find(mm, addr)) == NULL) {
				m.status |= q ? (1u << (28 + 2 * mm)) :
				    (1u << (27 + 2 * mm));
				m.nwords[mm][q] = 0;
				return;
			}
			memset(m.rdbuf[mm], 0, sizeof(m.rdbuf[mm]));
			/* the first byte is not data (Linux skips it) */
			((uint8_t *)m.rdbuf[mm])[0] = 0xa5;
			for (b = 0; b < len; b++)
				((uint8_t *)m.rdbuf[mm])[1 + b] = s->regs[s->ptr++];
			m.rdlevel[mm] = len / 4 + 1;
			m.rdpos[mm] = 0;
			m.status |= IRQ_RD_DONE(mm);
			break;
		case CCI_I2C_REPORT:
			if (w & CCI_I2C_REPORT_IRQ_EN)
				m.status |= q ? IRQ_Q1_REPORT(mm) : IRQ_Q0_REPORT(mm);
			i++;
			break;
		default:
			printf("model: bad command word %#x\n", w);
			fails++;
			i++;
		}
	}
	m.nwords[mm][q] = 0;
}

uint32_t
model_read(uint32_t reg)
{
	u_int mm, q;

	for (mm = 0; mm < 2; mm++)
		for (q = 0; q < 2; q++)
			if (reg == CCI_I2C_Q_CUR_WORD_CNT(mm, q))
				return (m.nwords[mm][q]);
	for (mm = 0; mm < 2; mm++) {
		if (reg == CCI_I2C_READ_BUF_LEVEL(mm))
			return (m.rdlevel[mm]);
		if (reg == CCI_I2C_READ_DATA(mm))
			return (m.rdbuf[mm][m.rdpos[mm]++]);
	}
	if (reg == CCI_IRQ_STATUS_0)
		return (m.status);
	if (reg == CCI_HW_VERSION)
		return (0x20050000);
	return (0);
}

void
model_write(uint32_t reg, uint32_t val)
{
	u_int mm, q;

	for (mm = 0; mm < 2; mm++)
		for (q = 0; q < 2; q++)
			if (reg == CCI_I2C_Q_LOAD_DATA(mm, q)) {
				assert(m.nwords[mm][q] < 64);
				m.words[mm][q][m.nwords[mm][q]++] = val;
				if (m.nlog < nitems(m.loaded_log))
					m.loaded_log[m.nlog++] = val;
				return;
			}
	if (reg == CCI_QUEUE_START) {
		for (mm = 0; mm < 2; mm++)
			for (q = 0; q < 2; q++)
				if (val & (1u << (mm * 2 + q)))
					run(mm, q);
	} else if (reg == CCI_IRQ_CLEAR_0)
		m.status &= ~val;
	else if (reg == CCI_HALT_REQ) {
		for (mm = 0; mm < 2; mm++)
			if (val & (1u << mm))
				m.status |= IRQ_HALT_ACK(mm);
	} else if (reg == CCI_RESET_CMD) {
		m.status |= IRQ_RST_DONE_ACK;
		if (val == CCI_RESET_CMD_MASK || val == CCI_RESET_CMD_M0_MASK)
			m.nwords[0][0] = m.nwords[0][1] = 0;
		if (val == CCI_RESET_CMD_MASK || val == CCI_RESET_CMD_M1_MASK)
			m.nwords[1][0] = m.nwords[1][1] = 0;
	}
}

void
model_delay(int us)
{
}

static int
reg_read(struct cci_softc *sc, u_int mm, uint8_t addr, uint16_t reg,
    uint8_t *buf, u_int len)
{
	uint8_t a[2] = { reg >> 8, reg & 0xff };
	struct iic_msg msgs[2] = {
		{ addr << 1, IIC_M_WR | IIC_M_NOSTOP, 2, a },
		{ addr << 1, IIC_M_RD, len, buf },
	};

	return (cci_transfer(sc, mm, msgs, 2));
}

static int
reg_write(struct cci_softc *sc, u_int mm, uint8_t addr, const uint8_t *buf,
    u_int len)
{
	struct iic_msg msg = { addr << 1, IIC_M_WR, len, (uint8_t *)buf };

	return (cci_transfer(sc, mm, &msg, 1));
}

int
main(void)
{
	struct cci_softc sc;
	uint8_t buf[16];
	int e;

	memset(&sc, 0, sizeof(sc));
	sc.present[0] = sc.present[1] = 1;
	sc.mode[0] = sc.mode[1] = 1;
	slaves[0].addr = 0x10;			/* the IMX582 */
	slaves[0].master = 0;
	slaves[0].regs[0x16] = 0x05;
	slaves[0].regs[0x17] = 0x82;
	slaves[1].addr = 0x2d;			/* the S5K4H7 */
	slaves[1].master = 1;
	slaves[1].regs[0x00] = 0x48;
	slaves[1].regs[0x01] = 0x7b;

	CHECK(cci_reset(&sc) == 0);
	cci_init(&sc);

	/* the chip IDs, as pro1x-cam reads them */
	memset(buf, 0, sizeof(buf));
	m.nlog = 0;
	e = reg_read(&sc, 0, 0x10, 0x0016, buf, 2);
	CHECK(e == 0 && buf[0] == 0x05 && buf[1] == 0x82);
	/* queue 0: SET_PARAM 0x10, WRITE 2 [00 16], REPORT; queue 1:
	 * SET_PARAM, READ 2 */
	CHECK(m.nlog == 5);
	CHECK(m.loaded_log[0] == (CCI_I2C_SET_PARAM | 0x10 << 4));
	CHECK(m.loaded_log[1] == (CCI_I2C_WRITE | 2 << 4 | 0x00 << 8 | 0x16 << 16));
	CHECK(m.loaded_log[2] == (CCI_I2C_REPORT | CCI_I2C_REPORT_IRQ_EN));
	CHECK(m.loaded_log[3] == (CCI_I2C_SET_PARAM | 0x10 << 4));
	CHECK(m.loaded_log[4] == (CCI_I2C_READ | 2 << 4));
	e = reg_read(&sc, 1, 0x2d, 0x0000, buf, 2);
	CHECK(e == 0 && buf[0] == 0x48 && buf[1] == 0x7b);

	/* MODE_SELECT = 1: one register a transfer */
	buf[0] = 0x01; buf[1] = 0x00; buf[2] = 0x01;
	m.nlog = 0;
	CHECK(reg_write(&sc, 0, 0x10, buf, 3) == 0);
	CHECK(slaves[0].regs[0x0100] == 1);
	CHECK(m.loaded_log[1] == (CCI_I2C_WRITE | 3 << 4 | 0x01 << 8 | 0x00 << 16 |
	    0x01u << 24));

	/* the limits: 11 bytes a write, 12 a read */
	uint8_t big[12] = { 0x30, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	CHECK(reg_write(&sc, 0, 0x10, big, 11) == 0);
	CHECK(slaves[0].regs[0x3000] == 1 && slaves[0].regs[0x3008] == 9);
	CHECK(reg_write(&sc, 0, 0x10, big, 12) == IIC_ENOTSUPP);
	memset(buf, 0, sizeof(buf));
	CHECK(reg_read(&sc, 0, 0x10, 0x3000, buf, 9) == 0);
	CHECK(buf[0] == 1 && buf[8] == 9);
	CHECK(reg_read(&sc, 0, 0x10, 0x3000, buf, 13) == IIC_ENOTSUPP);
	for (u_int n = 1; n <= 12; n++) {
		memset(buf, 0, sizeof(buf));
		CHECK(reg_read(&sc, 0, 0x10, 0x3000, buf, n) == 0);
		CHECK(buf[n - 1] == (n <= 9 ? n : 0));
	}

	/* nobody at 0x55: NACK, the master halted and reset, then fine again */
	CHECK(reg_read(&sc, 0, 0x55, 0x0000, buf, 2) == IIC_ENOACK);
	CHECK(sc.nacks == 1);
	CHECK(reg_read(&sc, 0, 0x10, 0x0016, buf, 2) == 0 && buf[1] == 0x82);
	/* the front sensor is on master 1 only */
	CHECK(reg_read(&sc, 0, 0x2d, 0x0000, buf, 2) == IIC_ENOACK);

	printf("test_cci: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return (fails != 0);
}
