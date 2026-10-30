/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-cam: the Pro1X cameras from user space (freebsd/camera/README.md).
 */
#ifndef _CAM_H_
#define	_CAM_H_

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>

#include "qcom_camss_ioctl.h"

#ifndef nitems
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif
#ifndef __printflike		/* the host tests on Linux */
#define	__printflike(f, a)	__attribute__((__format__(__printf__, f, a)))
#endif

extern int verbose;
void	msg(const char *fmt, ...) __printflike(1, 2);
void	vmsg(const char *fmt, ...) __printflike(1, 2);
void	msleep(u_int ms);

/* --- /dev/iicN (iic.c) ---------------------------------------------------- */
int	iic_open(int unit);
int	iic_rd16(int unit, uint8_t addr, uint16_t reg, uint8_t *buf, u_int len);
int	iic_wr16(int unit, uint8_t addr, uint16_t reg, const uint8_t *buf,
	    u_int len);
int	iic_rd8(int unit, uint8_t addr, uint8_t reg, uint8_t *buf, u_int len);
int	iic_wr8(int unit, uint8_t addr, uint8_t reg, const uint8_t *buf,
	    u_int len);

/* --- the board (power.c) ---------------------------------------------------- */
struct cam {
	int			fd;		/* /dev/camss0 */
	struct camss_info	info;
	int			pmic_up;
	int			rail_ref[3][16];	/* by type, index */
};

int	cam_open(struct cam *c, const char *path);
int	cam_find(struct cam *c, const char *name);	/* rear, front, 0, 1 */
const char *cam_name(struct cam *c, int s);
int	rail_on(struct cam *c, const struct camss_supply *s);
int	rail_off(struct cam *c, const struct camss_supply *s);
int	sensor_power(struct cam *c, int s, int on);
int	lens_power(struct cam *c, int s, int on);
uint32_t pm8008_grid_uv(u_int ldo, uint32_t min_uv);

/* --- the sensors (sensor.c) ---------------------------------------------------- */
struct sensor_mode;
struct sensor_ops {
	const char		*compatible;
	const char		*name;
	const char *const	*supplies;	/* the Linux driver's order */
	uint16_t		id_reg, id;
	const struct sensor_mode *modes;
	u_int			nmodes;
	const uint64_t		*link_freqs;
	const char		*bayer;		/* RGGB, GRBG, ... */
	int	(*start)(struct cam *, int, const struct sensor_mode *);
	int	(*stop)(struct cam *, int);
	int	(*exposure)(struct cam *, int, const struct sensor_mode *,
		    u_int lines, u_int gain);
	u_int	exp_min, exp_margin, gain_min, gain_max;
};

const struct sensor_ops *sensor_ops(struct cam *c, int s);
int	sensor_read_id(struct cam *c, int s, uint16_t *id);
int	sensor_write_regs(struct cam *c, int s, const void *regs, u_int n);
int	sensor_reg(struct cam *c, int s, uint16_t reg, int width, int wr,
	    uint32_t *val);
const struct sensor_mode *sensor_mode(const struct sensor_ops *o,
	    const char *name);
u_int	sensor_gain_code(const struct sensor_ops *o, double gain);
int	lens_move(struct cam *c, int s, int from, int to);
int	lens_status(struct cam *c, int s, uint8_t *st);

/* --- frames (capture.c, image.c, jpeg.c) ------------------------------------------- */
struct frame {
	uint32_t	width, height, stride;
	uint8_t		*data;		/* packed RAW10 as the TFE wrote it */
	uint32_t	sequence, flags;
};

int	capture(struct cam *c, int s, const struct sensor_mode *m, u_int skip,
	    u_int nframes, int (*cb)(struct cam *, int, struct frame *, void *),
	    void *arg);
void	raw10_unpack(const struct frame *f, uint16_t *out);
void	raw_stats(const uint16_t *px, uint32_t w, uint32_t h, const char *bayer,
	    double mean[4], uint32_t *maxv, double *clipped);
uint8_t	*debayer(const uint16_t *px, uint32_t w, uint32_t h, const char *bayer,
	    u_int black, int half, uint32_t *ow, uint32_t *oh);
void	rotate_rgb(uint8_t **rgb, uint32_t *w, uint32_t *h, int degrees);
int	png_write(FILE *fp, const uint8_t *px, uint32_t w, uint32_t h,
	    int channels, int bits);
int	jpeg_write(FILE *fp, const uint8_t *rgb, uint32_t w, uint32_t h,
	    int quality);
double	sharpness(const uint16_t *px, uint32_t w, uint32_t h);

/* --- a picture (shot.c) ------------------------------------------------------- */
struct shot_opts {
	const char	*out;		/* the JPEG */
	const char	*mode;		/* NULL: full size */
	int		af;		/* the contrast sweep first */
	int		focus;		/* lens position, -1: 100; out: used */
	int		quality;
	int		rotation;	/* -1000: pro1x-camera's */
	int		small;		/* also <out>-small.jpg */
};
void	shot_defaults(struct shot_opts *o);
int	take_picture(struct cam *c, int s, struct shot_opts *o);

#endif
