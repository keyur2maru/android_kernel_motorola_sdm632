// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM panel driver for the Motorola DJN 569 720x1512 video-mode DSI panel
 * used on the Moto G7 Play (channel, SDM632/msm8953).
 *
 * The DCS init/mode data was produced from the downstream vendor device tree
 * dsi-panel-mot-djn-569-hd-vid.dtsi with
 * https://github.com/msm8953-mainline/linux-mdss-dsi-panel-driver-generator
 * and ported to the drm_panel/mipi_dsi API present in this 4.9 tree
 * (drm_panel_init() single-arg form, get_modes(panel) using panel->connector,
 * struct mipi_dsi_driver .remove returning int) using
 * drivers/gpu/drm/panel/panel-sharp-ls043t1le01.c as the API template.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

struct djn_569 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *backlight;
	struct regulator_bulk_data supplies[3];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bklt_en_gpio;
	struct gpio_desc *hbm_gpio;
	bool prepared;
	bool init_failed;
	bool enabled;
};

static inline struct djn_569 *to_djn_569(struct drm_panel *panel)
{
	return container_of(panel, struct djn_569, panel);
}

/*
 * The downstream command table sends every manufacturer/CMD2 register,
 * including the 51/53/55 backlight block, as a generic LONG write
 * (dtype 29) regardless of payload size.  mipi_dsi_generic_write()
 * picks the generic SHORT type for 2-byte payloads, and this DDIC does
 * not latch the registers from short packets.  Force the long type so
 * the wire format matches the downstream stream byte for byte.
 */
static ssize_t djn_569_generic_long_write(struct mipi_dsi_device *dsi,
					  const void *payload, size_t size)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_GENERIC_LONG_WRITE,
		.tx_buf = payload,
		.tx_len = size,
	};

	if (!dsi->host->ops || !dsi->host->ops->transfer)
		return -ENOSYS;

	if (dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg.flags |= MIPI_DSI_MSG_USE_LPM;

	return dsi->host->ops->transfer(dsi->host, &msg);
}

#define dsi_generic_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = djn_569_generic_long_write(dsi, d, ARRAY_SIZE(d));\
		if (ret < 0)						\
			return ret;					\
	} while (0)

/*
 * Downstream reset waveform (qcom,mdss-dsi-reset-sequence <1 10>, <0 10>,
 * <1 5> + 7ms init delay): physically high 10ms, low 10ms, then high and
 * hold before the first command.  Drive raw line levels: the TDDI touch
 * MCU on the same die survives the probe-time logical-high write, so the
 * active-low flag on this gpio is not being applied and logical values
 * ended the sequence with the line low - the panel sat in hardware reset
 * through the whole init stream (touch drops off i2c at exactly that
 * moment and the power-mode readback returns nothing).
 */
static void djn_569_reset(struct djn_569 *ctx)
{
	gpiod_direction_output_raw(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_raw_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_raw_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(20000, 21000);
}

static int djn_569_on(struct djn_569 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	dsi_generic_write_seq(dsi, 0xff, 0x23);
	dsi_generic_write_seq(dsi, 0xfb, 0x01);
	dsi_generic_write_seq(dsi, 0x07, 0x20);
	dsi_generic_write_seq(dsi, 0x08, 0x06);
	dsi_generic_write_seq(dsi, 0x09, 0x00);
	dsi_generic_write_seq(dsi, 0xff, 0x24);
	dsi_generic_write_seq(dsi, 0xfb, 0x01);
	dsi_generic_write_seq(dsi, 0x5e, 0x11);
	dsi_generic_write_seq(dsi, 0x3b, 0x8c);
	dsi_generic_write_seq(dsi, 0x5b, 0x8c);
	dsi_generic_write_seq(dsi, 0x93, 0x08);
	dsi_generic_write_seq(dsi, 0x0a, 0x1c);
	dsi_generic_write_seq(dsi, 0x20, 0x1c);
	dsi_generic_write_seq(dsi, 0x86, 0x00);
	dsi_generic_write_seq(dsi, 0x80, 0x09);
	dsi_generic_write_seq(dsi, 0x81, 0x08);
	dsi_generic_write_seq(dsi, 0x82, 0x09);
	dsi_generic_write_seq(dsi, 0x83, 0x08);
	dsi_generic_write_seq(dsi, 0x84, 0x80);
	dsi_generic_write_seq(dsi, 0xff, 0x25);
	dsi_generic_write_seq(dsi, 0xfb, 0x01);
	dsi_generic_write_seq(dsi, 0x4c, 0x8c);
	dsi_generic_write_seq(dsi, 0x5e, 0x8c);
	dsi_generic_write_seq(dsi, 0x20, 0x8c);
	dsi_generic_write_seq(dsi, 0x27, 0x8c);
	dsi_generic_write_seq(dsi, 0x34, 0x8c);
	dsi_generic_write_seq(dsi, 0x43, 0x8c);
	dsi_generic_write_seq(dsi, 0x50, 0x8c);
	dsi_generic_write_seq(dsi, 0x62, 0x8c);
	dsi_generic_write_seq(dsi, 0x0a, 0x82);
	dsi_generic_write_seq(dsi, 0x0b, 0x9e);
	dsi_generic_write_seq(dsi, 0x0c, 0x01);
	dsi_generic_write_seq(dsi, 0xff, 0x26);
	dsi_generic_write_seq(dsi, 0xfb, 0x01);
	dsi_generic_write_seq(dsi, 0x64, 0x8c);
	dsi_generic_write_seq(dsi, 0x83, 0x8c);
	dsi_generic_write_seq(dsi, 0x68, 0x8c);
	dsi_generic_write_seq(dsi, 0x74, 0x8c);
	dsi_generic_write_seq(dsi, 0x7b, 0x8c);
	dsi_generic_write_seq(dsi, 0x87, 0x8c);
	dsi_generic_write_seq(dsi, 0xff, 0x10);
	dsi_generic_write_seq(dsi, 0xfb, 0x01);
	/*
	 * Set full brightness here in the LP init phase, not 0x00.  0x53=0x24
	 * enables the DCS backlight control block, but the brightness (0x51) is
	 * what the LED driver actually uses.  Runtime backlight updates run while
	 * video is streaming, and DCS commands cannot be transmitted during active
	 * video on this setup (the data lanes carry the pixel stream), so the
	 * runtime 0x51 write stalls -- leaving the panel dark if init left it 0.
	 * Program it on here while the LP init path works; runtime updates are
	 * then best-effort refinements, not required for a lit panel.
	 */
	dsi_generic_write_seq(dsi, 0x51, 0xff);
	dsi_generic_write_seq(dsi, 0x53, 0x24);
	dsi_generic_write_seq(dsi, 0x55, 0x01);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;
	msleep(101);

	return 0;
}

static int djn_569_off(struct djn_569 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		return ret;
	msleep(20);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		return ret;
	msleep(100);

	return 0;
}

static int djn_569_disable(struct drm_panel *panel)
{
	struct djn_569 *ctx = to_djn_569(panel);
	int ret;

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	/* The off commands go out from disable(), while the video engine
	 * is still running, for the same command-DMA scheduling reason
	 * the init sequence is sent from enable().
	 */
	ret = djn_569_off(ctx);
	if (ret < 0)
		dev_err(panel->dev, "failed to set panel off: %d\n", ret);

	ctx->enabled = false;

	return 0;
}

static int djn_569_unprepare(struct drm_panel *panel)
{
	struct djn_569 *ctx = to_djn_569(panel);

	if (!ctx->prepared)
		return 0;

	if (ctx->hbm_gpio)
		gpiod_set_value_cansleep(ctx->hbm_gpio, 0);
	if (ctx->bklt_en_gpio)
		gpiod_set_value_cansleep(ctx->bklt_en_gpio, 0);
	gpiod_set_raw_value_cansleep(ctx->reset_gpio, 0);

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	ctx->prepared = false;

	return 0;
}

static int djn_569_prepare(struct drm_panel *panel)
{
	struct djn_569 *ctx = to_djn_569(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	/*
	 * vddio powers the TDDI logic; vsp/vsn are the LCDB +/-5.5V glass
	 * bias rails.  The bootloader leaves them on for the splash, but
	 * nothing held a reference, so the regulator late cleanup switched
	 * the bias off ~30s after boot and the panel went permanently dark.
	 * The downstream host enables vddio -> vsp -> vsn before the reset
	 * pulse (lp11-lcdb-reset).
	 */
	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);

	djn_569_reset(ctx);

	ctx->prepared = true;

	return 0;
}

static int djn_569_enable(struct drm_panel *panel)
{
	struct djn_569 *ctx = to_djn_569(panel);
	int ret;

	if (ctx->enabled)
		return 0;

	/* The init sequence is sent from enable(), after the host has
	 * started the video engine, not from prepare(): on this
	 * controller the command DMA fetch is only serviced when the
	 * transmit scheduler has a slot, which the running video engine
	 * provides (BLLP insertion).  With the link idle the DMA engine
	 * sits busy forever and never fetches.  The downstream driver
	 * sends every panel command with the video engine running.
	 */
	if (ctx->init_failed)
		return -ENODEV;

	ret = djn_569_on(ctx);
	if (ret < 0) {
		dev_err(panel->dev, "failed to initialize panel: %d\n", ret);
		/* Latch the failure: every retry parks another DMA
		 * transaction on the fabric and eventually starves other
		 * bus masters (USB dies), so fail fast and leave the link
		 * quiet after the first attempt.
		 */
		ctx->init_failed = true;
		return ret;
	}

	/* Non-fatal init receipt check: 0x9c means sleep-out and
	 * display-on landed.  HS read, as the clock lane is force-HS
	 * while video runs.
	 */
	{
		u8 pwr = 0;

		ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
		ret = mipi_dsi_dcs_read(ctx->dsi, MIPI_DCS_GET_POWER_MODE,
					&pwr, 1);
		ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;
		dev_info(panel->dev, "power mode after init: ret=%d 0x%02x\n",
			 ret, pwr);
	}

	if (ctx->bklt_en_gpio)
		gpiod_set_value_cansleep(ctx->bklt_en_gpio, 1);

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}

static const struct drm_display_mode djn_569_mode = {
	.clock = (720 + 220 + 12 + 72) * (1512 + 8 + 2 + 8) * 60 / 1000,
	.hdisplay = 720,
	.hsync_start = 720 + 220,
	.hsync_end = 720 + 220 + 12,
	.htotal = 720 + 220 + 12 + 72,
	.vdisplay = 1512,
	.vsync_start = 1512 + 8,
	.vsync_end = 1512 + 8 + 2,
	.vtotal = 1512 + 8 + 2 + 8,
	.vrefresh = 60,
};

static int djn_569_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &djn_569_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 64;
	panel->connector->display_info.height_mm = 128;

	return 1;
}

static const struct drm_panel_funcs djn_569_panel_funcs = {
	.disable = djn_569_disable,
	.unprepare = djn_569_unprepare,
	.prepare = djn_569_prepare,
	.enable = djn_569_enable,
	.get_modes = djn_569_get_modes,
};

static int djn_569_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = bl->props.brightness;
	int ret;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	/*
	 * Runtime backlight updates run while the video stream is active, and on
	 * this board the clock lane is force-HS during video (dsi_op_mode_config
	 * CLKLN_HS_FORCE_REQUEST) because the 14nm PHY won't auto-engage HS for a
	 * non-continuous clock.  With the clock lane held HS the data lanes cannot
	 * do an LP escape, so an LP DCS write here stalls (-ETIMEDOUT).  Send the
	 * brightness DCS in HS instead -- it has a valid HS byte clock during
	 * video.  (The init-time brightness=0 write in djn_569_on() stays LP; it
	 * runs in the LP init phase before the clock lane is forced HS.)
	 */
	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness(dsi, brightness);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct backlight_ops djn_569_bl_ops = {
	.update_status = djn_569_bl_update_status,
};

static struct backlight_device *
djn_569_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 255,
		.max_brightness = 255,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &djn_569_bl_ops, &props);
}

static int djn_569_add(struct djn_569 *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vsp";
	ctx->supplies[2].supply = "vsn";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to get supplies: %d\n", ret);
		return ret;
	}

	/*
	 * Take a reference already at probe: the bootloader hands the panel
	 * over initialized and lit, and the regulator late cleanup would
	 * otherwise cut the LCDB bias out from under that live panel seconds
	 * before the first prepare, leaving the TDDI in an undefined state.
	 */
	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable supplies at probe: %d\n", ret);
		return ret;
	}

	/* ASIS: do not glitch the line at probe - the bootloader leaves the
	 * panel out of reset with the splash live.
	 */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "failed to get reset-gpios: %d\n", ret);
		return ret;
	}

	ctx->bklt_en_gpio = devm_gpiod_get_optional(dev, "bklt-en",
						    GPIOD_OUT_LOW);
	if (IS_ERR(ctx->bklt_en_gpio)) {
		ret = PTR_ERR(ctx->bklt_en_gpio);
		dev_err(dev, "failed to get bklt-en-gpios: %d\n", ret);
		return ret;
	}

	ctx->hbm_gpio = devm_gpiod_get_optional(dev, "hbm-en", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->hbm_gpio)) {
		ret = PTR_ERR(ctx->hbm_gpio);
		dev_err(dev, "failed to get hbm-en-gpios: %d\n", ret);
		return ret;
	}

	ctx->backlight = djn_569_create_backlight(ctx->dsi);
	if (IS_ERR(ctx->backlight)) {
		ret = PTR_ERR(ctx->backlight);
		dev_err(dev, "failed to create backlight: %d\n", ret);
		return ret;
	}

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &djn_569_panel_funcs;

	return drm_panel_add(&ctx->panel);
}

static int djn_569_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct djn_569 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/* Non-burst sync-event, matching the traffic mode the bootloader
	 * and the downstream stack run this panel with (VID_CFG0 live
	 * value 0x80009130: traffic mode 1 + last-line-interleave).
	 */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ret = djn_569_add(ctx);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static int djn_569_remove(struct mipi_dsi_device *dsi)
{
	struct djn_569 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_detach(&ctx->panel);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id djn_569_of_match[] = {
	{ .compatible = "mot,djn-569-hd-vid" },
	{ }
};
MODULE_DEVICE_TABLE(of, djn_569_of_match);

static struct mipi_dsi_driver djn_569_driver = {
	.probe = djn_569_probe,
	.remove = djn_569_remove,
	.driver = {
		.name = "panel-mot-djn-569",
		.of_match_table = djn_569_of_match,
	},
};
module_mipi_dsi_driver(djn_569_driver);

MODULE_AUTHOR("Keyur Maru <kd2maru@gmail.com>");
MODULE_DESCRIPTION("DRM driver for the Motorola DJN 569 720x1512 DSI panel");
MODULE_LICENSE("GPL v2");
