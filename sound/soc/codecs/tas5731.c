#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/tas57xx.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of_gpio.h>

#include "tas5731.h"

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
static void tas5731_early_suspend(struct early_suspend *h);
static void tas5731_late_resume(struct early_suspend *h);
#endif

#define tas5731_RATES (SNDRV_PCM_RATE_8000 | \
			   SNDRV_PCM_RATE_11025 | \
			   SNDRV_PCM_RATE_16000 | \
			   SNDRV_PCM_RATE_22050 | \
			   SNDRV_PCM_RATE_32000 | \
			   SNDRV_PCM_RATE_44100 | \
			   SNDRV_PCM_RATE_48000)

#define tas5731_FORMATS \
	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE | \
	 SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S20_3BE | \
	 SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_BE | \
	 SNDRV_PCM_FMTBIT_S32_LE)


/* Power-up register defaults */
struct reg_default tas5731_reg_defaults[0xff] = {
	{0x00, 0x6c},
	{0x01, 0x70},
	{0x02, 0x00},
	{0x03, 0xA0},
	{0x04, 0x05},
	{0x05, 0x40},
	{0x06, 0x00},
	{0x07, 0xFF},
	{0x08, 0x30},
	{0x09, 0x30},
	{0x0A, 0xFF},
	{0x0B, 0x00},
	{0x0C, 0x00},
	{0x0D, 0x00},
	{0x0E, 0x91},
	{0x10, 0x00},
	{0x11, 0x02},
	{0x12, 0xAC},
	{0x13, 0x54},
	{0x14, 0xAC},
	{0x15, 0x54},
	{0x16, 0x00},
	{0x17, 0x00},
	{0x18, 0x00},
	{0x19, 0x00},
	{0x1A, 0x30},
	{0x1B, 0x0F},
	{0x1C, 0x82},
	{0x1D, 0x02}
};


static u8 TAS5731_drc1_table[3][8] = {
	/* 0x3A drc1_ae */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	/* 0x3B drc1_aa */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	/* 0x3C drc1_ad */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

static u8 TAS5731_drc2_table[3][8] = {
	/* 0x3D drc2_ae */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	/* 0x3E drc2_aa */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	/* 0x3F drc2_ad */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

static u8 tas5731_drc1_tko_table[3][4] = {
	/* 0x40 drc1_t */
	{ 0x00, 0x00, 0x00, 0x00 },
	/* 0x41 drc1_k */
	{ 0x00, 0x00, 0x00, 0x00 },
	/* 0x42 drc1_o */
	{ 0x00, 0x00, 0x00, 0x00 }
};

static u8 tas5731_drc2_tko_table[3][4] = {
	/* 0x43 drc2_t */
	{ 0x00, 0x00, 0x00, 0x00 },
	/* 0x44 drc2_k */
	{ 0x00, 0x00, 0x00, 0x00 },
	/* 0x45 drc2_o */
	{ 0x00, 0x00, 0x00, 0x00 }
};


/* codec private data */
struct tas5731_priv {
	struct snd_soc_codec *codec;
	struct tas57xx_platform_data *pdata;

	struct regmap *control_data;

	/*Platform provided EQ configuration */
	int num_eq_conf_texts;
	const char **eq_conf_texts;
	int eq_cfg;
	struct soc_enum eq_conf_enum;
	unsigned char Ch1_vol;
	unsigned char Ch2_vol;
	unsigned char Master_vol;
	unsigned int mclk;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

static const DECLARE_TLV_DB_SCALE(mvol_tlv, -12700, 50, 1);
static const DECLARE_TLV_DB_SCALE(chvol_tlv, -10300, 50, 1);

static const struct snd_kcontrol_new tas5731_snd_controls[] = {
	SOC_SINGLE_TLV("Master Volume", DDX_MASTER_VOLUME, 0,
			   0xff, 1, mvol_tlv),
	SOC_SINGLE_TLV("Ch1 Volume", DDX_CHANNEL1_VOL, 0,
			   0xff, 1, chvol_tlv),
	SOC_SINGLE_TLV("Ch2 Volume", DDX_CHANNEL2_VOL, 0,
			   0xff, 1, chvol_tlv),
	SOC_SINGLE("Ch1 Switch", DDX_SOFT_MUTE, 0, 1, 1),
	SOC_SINGLE("Ch2 Switch", DDX_SOFT_MUTE, 1, 1, 1),
	SOC_SINGLE_RANGE("Fine Master Volume", DDX_CHANNEL3_VOL, 0,
			   0x80, 0x83, 0),
};

static int tas5731_set_dai_sysclk(struct snd_soc_dai *codec_dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);

	tas5731->mclk = freq;
	/* 0x74 = 512fs; 0x6c = 256fs */
	if (freq == 512 * 48000)
		snd_soc_write(codec, DDX_CLOCK_CTL, 0x74);
	else
		snd_soc_write(codec, DDX_CLOCK_CTL, 0x6c);
	return 0;
}

static int tas5731_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_NB_IF:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int tas5731_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	unsigned int rate;

	rate = params_rate(params);
	pr_debug("rate: %u\n", rate);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_BE:
		pr_debug("24bit\n");
	/* fall through */
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_S20_3LE:
	case SNDRV_PCM_FORMAT_S20_3BE:
		pr_debug("20bit\n");

		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S16_BE:
		pr_debug("16bit\n");

		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int tas5731_set_bias_level(struct snd_soc_codec *codec,
				  enum snd_soc_bias_level level)
{
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(codec);

	pr_debug("level = %d\n", level);
	switch (level) {
	case SND_SOC_BIAS_ON:
		break;

	case SND_SOC_BIAS_PREPARE:
		/* Full power on */
		break;

	case SND_SOC_BIAS_STANDBY:
		break;

	case SND_SOC_BIAS_OFF:
		/* The chip runs through the power down sequence for us. */
		break;
	}
	dapm->bias_level = level;
	return 0;
}

static const struct snd_soc_dai_ops tas5731_dai_ops = {
	.hw_params = tas5731_hw_params,
	.set_sysclk = tas5731_set_dai_sysclk,
	.set_fmt = tas5731_set_dai_fmt,
};

static struct snd_soc_dai_driver tas5731_dai = {
	.name = "tas5731",
	.playback = {
		.stream_name = "HIFI Playback",
		.channels_min = 2,
		.channels_max = 8,
		.rates = tas5731_RATES,
		.formats = tas5731_FORMATS,
	},
	.ops = &tas5731_dai_ops,
};

static int tas5731_set_master_vol(struct snd_soc_codec *codec)
{
	struct tas57xx_platform_data *pdata = dev_get_platdata(codec->dev);

	/* using user BSP defined master vol config; */
	if (pdata && pdata->custom_master_vol) {
		pr_debug("tas5731_set_master_vol::%d\n",
			pdata->custom_master_vol);
		snd_soc_write(codec, DDX_MASTER_VOLUME,
				  (0xff - pdata->custom_master_vol));
	} else {
		pr_debug
			("get dtd master_vol failed:using default setting\n");
		snd_soc_write(codec, DDX_MASTER_VOLUME, 0x30);
	}

	return 0;
}

/* tas5731 DRC for channel L/R */
static int tas5731_set_drc1(struct snd_soc_codec *codec)
{
	int i = 0, j = 0;
	u8 *p = NULL;
	struct tas57xx_platform_data *pdata = dev_get_platdata(codec->dev);

	if (pdata && pdata->custom_drc1_table
		&& pdata->custom_drc1_table_len == 24) {
		p = pdata->custom_drc1_table;
		for (i = 0; i < 3; i++) {
			for (j = 0; j < 8; j++)
				TAS5731_drc1_table[i][j] = p[i * 8 + j];

			regmap_raw_write(codec->control_data, DDX_DRC1_AE + i,
					 TAS5731_drc1_table[i], 8);
		}
	} else {
		return -1;
	}

	if (pdata && pdata->custom_drc1_tko_table
		&& pdata->custom_drc1_tko_table_len == 12) {
		p = pdata->custom_drc1_tko_table;
		for (i = 0; i < 3; i++) {
			for (j = 0; j < 4; j++)
				tas5731_drc1_tko_table[i][j] = p[i * 4 + j];

			regmap_raw_write(codec->control_data, DDX_DRC1_T + i,
					 tas5731_drc1_tko_table[i], 4);
		}
	} else {
		return -1;
	}

	return 0;
}

static int tas5731_set_drc2(struct snd_soc_codec *codec)
{
	int i = 0, j = 0;
	u8 *p = NULL;
	struct tas57xx_platform_data *pdata = dev_get_platdata(codec->dev);

	if (pdata && pdata->custom_drc2_table
		&& pdata->custom_drc2_table_len == 24) {
		p = pdata->custom_drc2_table;
		for (i = 0; i < 3; i++) {
			for (j = 0; j < 8; j++)
				TAS5731_drc2_table[i][j] = p[i * 8 + j];

			regmap_raw_write(codec->control_data, DDX_DRC2_AE + i,
					 TAS5731_drc2_table[i], 8);
		}
	} else {
		return -1;
	}

	if (pdata && pdata->custom_drc2_tko_table
		&& pdata->custom_drc2_tko_table_len == 12) {
		p = pdata->custom_drc2_tko_table;
		for (i = 0; i < 3; i++) {
			for (j = 0; j < 4; j++)
				tas5731_drc2_tko_table[i][j] = p[i * 4 + j];

			regmap_raw_write(codec->control_data, DDX_DRC2_T + i,
					 tas5731_drc2_tko_table[i], 4);
		}
	} else {
		return -1;
	}

	return 0;
}


static int tas5731_set_drc(struct snd_soc_codec *codec)
{
	struct tas57xx_platform_data *pdata = dev_get_platdata(codec->dev);
	char drc_mask = 0;
	u8 tas5731_drc_ctl_table[] = { 0x00, 0x00, 0x00, 0x00 };

	if (pdata && pdata->enable_ch1_drc
		&& pdata->enable_ch2_drc && pdata->drc_enable) {
		drc_mask |= 0x03;
		tas5731_drc_ctl_table[3] = drc_mask;
		tas5731_set_drc1(codec);
		tas5731_set_drc2(codec);
		regmap_raw_write(codec->control_data, DDX_DRC_CTL,
			tas5731_drc_ctl_table, 4);

		return 0;
	}

	return -1;
}

static int tas5731_set_eq_biquad(struct snd_soc_codec *codec)
{
	int i = 0, j = 0, k = 0;
	u8 *p = NULL;
	u8 addr;
	u8 tas5731_bq_table[20];
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);
	struct tas57xx_platform_data *pdata = tas5731->pdata;
	struct tas57xx_eq_cfg *cfg;

	if (!pdata)
		return 0;

	cfg = pdata->eq_cfgs;
	if (!(cfg))
		return 0;

	p = cfg[tas5731->eq_cfg].regs;

	/*
	 * EQ1: 0x29~0x2F, 0x58~0x59
	 * EQ2: 0x30~0x36, 0x5c~0x5d
	 */
	for (i = 0; i < 2; i++) {
		for (j = 0; j < 9; j++) {
			if (j < 7) {
				addr = (DDX_CH1_BQ_0 + i * 7 + j);
				for (k = 0; k < 20; k++)
					tas5731_bq_table[k] =
						p[i * 9 * 20 + j * 20 + k];
				regmap_raw_write(codec->control_data, addr,
						tas5731_bq_table, 20);
			} else {
				addr = (DDX_CH1_BQ_7 + i * 4 + j - 7);
				for (k = 0; k < 20; k++)
					tas5731_bq_table[k] =
						p[i * 9 * 20 + j * 20 + k];
				regmap_raw_write(codec->control_data, addr,
						tas5731_bq_table, 20);
			}
		}
	}

	return 0;
}

static int tas5731_put_eq_enum(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = snd_soc_component_to_codec(component);
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);
	struct tas57xx_platform_data *pdata = tas5731->pdata;
	int value = ucontrol->value.integer.value[0];

	if (value >= pdata->num_eq_cfgs)
		return -EINVAL;

	tas5731->eq_cfg = value;
	tas5731_set_eq_biquad(codec);

	return 0;
}

static int tas5731_get_eq_enum(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = snd_soc_component_to_codec(component);
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.enumerated.item[0] = tas5731->eq_cfg;

	return 0;
}

static const char *const eq_enable_texts[] = {
	"Enable",
	"Disable",
};
static int current_eq_status;
static const struct soc_enum eq_en_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0, ARRAY_SIZE(eq_enable_texts),
	eq_enable_texts);

static int tas5731_get_eq_enable(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.enumerated.item[0] = current_eq_status;
	return 0;
}

static int tas5731_set_eq_enable(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = snd_soc_component_to_codec(component);
	u8 tas5731_eq_enable_table[]   = { 0x00, 0x00, 0x00, 0x00 };
	u8 tas5731_eq_disable_table[]  = { 0x00, 0x00, 0x00, 0x80 };
	u8 tas5731_drc_enable_table[]  = { 0x00, 0x00, 0x00, 0x03 };
	u8 tas5731_drc_disable_table[] = { 0x00, 0x00, 0x00, 0x00 };

	if (ucontrol->value.enumerated.item[0] == 0) {
		regmap_raw_write(codec->control_data,
			DDX_BANKSWITCH_AND_EQCTL,
			tas5731_eq_enable_table, 4);
		regmap_raw_write(codec->control_data,
			DDX_DRC_CTL,
			tas5731_drc_enable_table, 4);
		current_eq_status = 0;
	} else if (ucontrol->value.enumerated.item[0] == 1) {
		regmap_raw_write(codec->control_data,
			DDX_BANKSWITCH_AND_EQCTL,
			tas5731_eq_disable_table, 4);
		regmap_raw_write(codec->control_data,
			DDX_DRC_CTL,
			tas5731_drc_disable_table, 4);
		current_eq_status = 1;
	}

	return 0;
}

static int tas5731_set_eq(struct snd_soc_codec *codec)
{
	int i = 0, ret = 0;
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);
	struct tas57xx_platform_data *pdata = tas5731->pdata;
	u8 tas5731_eq_ctl_table[] = { 0x00, 0x00, 0x00, 0x80 };
	struct tas57xx_eq_cfg *cfg = pdata->eq_cfgs;
	struct snd_kcontrol_new control_dbg =
		SOC_ENUM_EXT("EQ/DRC Enable", eq_en_enum,
				tas5731_get_eq_enable,
				tas5731_set_eq_enable);

	if (!pdata || !pdata->eq_enable)
		return -ENOENT;

	ret = snd_soc_add_codec_controls(codec, &control_dbg, 1);
	if (ret != 0)
		dev_err(codec->dev, "Fail to add EQ Enable control: %d\n", ret);
	if (pdata->num_eq_cfgs && (tas5731->eq_conf_texts == NULL)) {
		struct snd_kcontrol_new control =
			SOC_ENUM_EXT("EQ Mode", tas5731->eq_conf_enum,
				tas5731_get_eq_enum, tas5731_put_eq_enum);

		tas5731->eq_conf_texts =
			kzalloc(sizeof(char *) * pdata->num_eq_cfgs,
				GFP_KERNEL);
		if (!tas5731->eq_conf_texts) {
			dev_err(codec->dev,
				"Fail to allocate %d EQ config tests\n",
				pdata->num_eq_cfgs);
			return -ENOMEM;
		}

		for (i = 0; i < pdata->num_eq_cfgs; i++)
			tas5731->eq_conf_texts[i] = cfg[i].name;

		//porting, no max
		tas5731->eq_conf_enum.items = pdata->num_eq_cfgs;
		tas5731->eq_conf_enum.texts = tas5731->eq_conf_texts;

		ret = snd_soc_add_codec_controls(codec, &control, 1);
		if (ret != 0)
			dev_err(codec->dev, "Fail to add EQ mode control: %d\n",
				ret);
	}

	tas5731_set_eq_biquad(codec);

	tas5731_eq_ctl_table[3] &= 0x7F;
	regmap_raw_write(codec->control_data, DDX_BANKSWITCH_AND_EQCTL,
			 tas5731_eq_ctl_table, 4);

	return 0;
}

#include "xiaomi_tas5731_dump.c"

static int tas5731_customer_init(struct snd_soc_codec *codec)
{
	int i = 0;
	u8 data[4] = {0x00, 0x00, 0x00, 0x00};
	struct tas57xx_platform_data *pdata = dev_get_platdata(codec->dev);

	if (pdata && pdata->init_regs) {
		if (pdata->num_init_regs != 4) {
			dev_err(codec->dev, "num_init_regs = %d\n",
				pdata->num_init_regs);
		} else {
			for (i = 0; i < pdata->num_init_regs; i++)
				data[i] = pdata->init_regs[i];

			regmap_raw_write(codec->control_data,
				DDX_INPUT_MUX, data, 4);
		}
	}

	return 0;
}

static int reset_tas5731_GPIO(struct snd_soc_codec *codec)
{
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);
	struct tas57xx_platform_data *pdata = tas5731->pdata;

	if (pdata->pdn_pin != 0 && pdata->reset_pin != 0) {

		/* please refer to page 31, tas5731 spec
		 * TODO:
		 * there some mistake in timings sequence
		 * need to connect TI's FAE and correct it
		 */

		/* pdn active */
		if (pdata->pdn_pin_active_low)
			gpio_direction_output(pdata->pdn_pin,
				GPIOF_OUT_INIT_LOW);
		else
			gpio_direction_output(pdata->pdn_pin,
				GPIOF_OUT_INIT_HIGH);

		udelay(1000);

		/* reset active */
		if (pdata->reset_pin_active_low)
			gpio_direction_output(pdata->reset_pin,
				GPIOF_OUT_INIT_LOW);
		else
			gpio_direction_output(pdata->reset_pin,
				GPIOF_OUT_INIT_HIGH);

		udelay(1000);

		/* Power-up sequences */

		/* pdn inactive */
		if (pdata->pdn_pin_active_low)
			gpio_direction_output(pdata->pdn_pin,
				GPIOF_OUT_INIT_HIGH);
		else
			gpio_direction_output(pdata->pdn_pin,
				GPIOF_OUT_INIT_LOW);

		mdelay(10);

		/* reset inactive */
		if (pdata->reset_pin_active_low)
			gpio_direction_output(pdata->reset_pin,
				GPIOF_OUT_INIT_HIGH);
		else
			gpio_direction_output(pdata->reset_pin,
				GPIOF_OUT_INIT_LOW);

		mdelay(15);
	}

	return 0;
}

static int tas5731_init(struct snd_soc_codec *codec)
{
	unsigned char burst_data[][4] = {
		{ 0x00, 0x01, 0x77, 0x72 },
		{ 0x00, 0x00, 0x43, 0x03 },
		{ 0x01, 0x13, 0x02, 0x45 },
	};
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);

	reset_tas5731_GPIO(codec);

	dev_info(codec->dev, "tas5731_init!\n");
	snd_soc_write(codec, DDX_OSC_TRIM, 0x00);

	// porting, why here have 50 ms delay?
	msleep(50);

	snd_soc_write(codec, DDX_CLOCK_CTL, 0x6c);
	snd_soc_write(codec, DDX_SYS_CTL_1, 0xa0);
	snd_soc_write(codec, DDX_SERIAL_DATA_INTERFACE, 0x05);
	snd_soc_write(codec, DDX_BKND_ERR, 0x02);

	regmap_raw_write(codec->control_data, DDX_INPUT_MUX, burst_data[0], 4);
	regmap_raw_write(codec->control_data, DDX_CH4_SOURCE_SELECT,
			 burst_data[1], 4);
	regmap_raw_write(codec->control_data, DDX_PWM_MUX, burst_data[2], 4);

	/*drc */
	if ((tas5731_set_drc(codec)) < 0)
		dev_err(codec->dev, "fail to set tas5731 drc!\n");
	/*eq */
	if ((tas5731_set_eq(codec)) < 0)
		dev_err(codec->dev, "fail to set tas5731 eq!\n");

	/*init */
	if ((tas5731_customer_init(codec)) < 0)
		dev_err(codec->dev, " fail to set tas5731 customer init!\n");

	snd_soc_write(codec, DDX_VOLUME_CONFIG, 0xD1);
	snd_soc_write(codec, DDX_START_STOP_PERIOD, 0x0F);
	snd_soc_write(codec, DDX_PWM_SHUTDOWN_GROUP, 0x30);
	/* if pwr is greater than 18V, the modulation limit must set to 93.8% */
	snd_soc_write(codec, DDX_MODULATION_LIMIT, 0x07);

	/* exit shutdown sequence, need to wait 1ms + 1.3 x T(start) */
	snd_soc_write(codec, DDX_SYS_CTL_2, 0x00);
	msleep(170);

	/*normal operation */
	if ((tas5731_set_master_vol(codec)) < 0)
		dev_err(codec->dev, "fail to set tas5731 master vol!\n");

	snd_soc_write(codec, DDX_CHANNEL1_VOL, tas5731->Ch1_vol);
	snd_soc_write(codec, DDX_CHANNEL2_VOL, tas5731->Ch2_vol);
	snd_soc_write(codec, DDX_SOFT_MUTE, 0x00);
	snd_soc_write(codec, DDX_CHANNEL3_VOL, 0x80);

	return 0;
}

static int tas5731_probe(struct snd_soc_codec *codec)
{
	int ret;
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);
	struct tas57xx_platform_data *pdata = tas5731->pdata;

#ifdef CONFIG_HAS_EARLYSUSPEND
	tas5731->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	tas5731->early_suspend.suspend = tas5731_early_suspend;
	tas5731->early_suspend.resume = tas5731_late_resume;
	tas5731->early_suspend.param = codec;
	register_early_suspend(&(tas5731->early_suspend));
#endif

	tas5731->codec = codec;
	codec->dev->platform_data = pdata;
	codec->control_data = tas5731->control_data;

 	tas5731_init(codec);

	ret = sysfs_create_group(&codec->dev->kobj,
		&tas5731_group);
	if (ret < 0)
		dev_err(codec->dev, "Create sys fs node fail: %d\n", ret);
	else {
		if (pdata->cls_spk) {
			ret = class_compat_create_link(pdata->cls_spk,
				codec->dev,
				codec->dev->parent);
			if (ret)
				dev_warn(codec->dev,
					"Failed to create compatibility class link\n");
		}
	}

	return 0;
}

static int tas5731_remove(struct snd_soc_codec *codec)
{
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);

	unregister_early_suspend(&(tas5731->early_suspend));
#endif

	return 0;
}

#ifdef CONFIG_PM
static int tas5731_suspend(struct snd_soc_codec *codec)
{
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);
	struct tas57xx_platform_data *pdata = dev_get_platdata(codec->dev);

	dev_info(codec->dev, "tas5731_suspend!\n");

	if (pdata && pdata->suspend_func)
		pdata->suspend_func();

	/*save volume */
	tas5731->Ch1_vol = snd_soc_read(codec, DDX_CHANNEL1_VOL);
	tas5731->Ch2_vol = snd_soc_read(codec, DDX_CHANNEL2_VOL);
	tas5731->Master_vol = snd_soc_read(codec, DDX_MASTER_VOLUME);
	tas5731_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static int tas5731_resume(struct snd_soc_codec *codec)
{
	struct tas5731_priv *tas5731 = snd_soc_codec_get_drvdata(codec);
	struct tas57xx_platform_data *pdata = dev_get_platdata(codec->dev);

	dev_info(codec->dev, "tas5731_resume!\n");

	if (pdata && pdata->resume_func)
		pdata->resume_func();

	tas5731_init(codec);
	snd_soc_write(codec, DDX_CHANNEL1_VOL, tas5731->Ch1_vol);
	snd_soc_write(codec, DDX_CHANNEL2_VOL, tas5731->Ch2_vol);
	snd_soc_write(codec, DDX_MASTER_VOLUME, tas5731->Master_vol);
	tas5731_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	return 0;
}
#else
#define tas5731_suspend NULL
#define tas5731_resume NULL
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void tas5731_early_suspend(struct early_suspend *h)
{
}

static void tas5731_late_resume(struct early_suspend *h)
{
}
#endif

static const struct snd_soc_dapm_widget tas5731_dapm_widgets[] = {
	SND_SOC_DAPM_DAC("DAC", "HIFI Playback", SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_codec_driver tas5731_codec = {
	.probe = tas5731_probe,
	.remove = tas5731_remove,
	.suspend = tas5731_suspend,
	.resume = tas5731_resume,
	.set_bias_level = tas5731_set_bias_level,
	.component_driver = {
		.controls = tas5731_snd_controls,
		.num_controls = ARRAY_SIZE(tas5731_snd_controls),
		.dapm_widgets = tas5731_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(tas5731_dapm_widgets),
	}
};

static const struct regmap_config tas5731_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = 0xff,
	.reg_defaults = tas5731_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tas5731_reg_defaults),
	.cache_type = REGCACHE_NONE,
};

// starting get info from dts
static char drc1_table[15] = "drc1_table_0";
static char drc1_tko_table[20] = "drc1_tko_table_0";
static char drc2_table[15] = "drc2_table_0";
static char drc2_tko_table[20] = "drc2_tko_table_0";
static int __init drc_type_select(char *s)
{
	char *sel = s;

	if (s != NULL) {
		sprintf(drc1_table, "%s%s", "drc1_table_", sel);
		sprintf(drc1_tko_table, "%s%s", "drc1_tko_table_", sel);
		sprintf(drc2_table, "%s%s", "drc2_table_", sel);
		sprintf(drc2_tko_table, "%s%s", "drc2_tko_table_", sel);
		pr_info("select drc type: %s\n", sel);
	}
	return 0;
}
__setup("amp_drc_type=", drc_type_select);

static char table[10] = "table_0";
static char woofer1[10] = "woofer1";
static char woofer2[10] = "woofer2";
static char null[10] = "null";
static char sub_bq_table[20] = "sub_bq_table_0";
static int __init eq_type_select(char *s)
{
	char *sel = s;

	if (s != NULL) {
		sprintf(table, "%s%s", "table_", sel);
		sprintf(sub_bq_table, "%s%s", "sub_bq_table_", sel);
		pr_info("select eq type: %s\n", sel);
	}
	return 0;
}
__setup("amp_eq_type=", eq_type_select);

static void *alloc_and_get_data_array(struct device_node *p_node, char *str,
					  int *lenp)
{
	int ret = 0, length = 0;
	char *p = NULL;

	if (of_find_property(p_node, str, &length) == NULL) {
		pr_err("DTD of %s not found!\n", str);
		goto exit;
	}
	pr_debug("prop name=%s,length=%d\n", str, length);
	p = kcalloc(length, sizeof(char *), GFP_KERNEL);
	if (p == NULL) {
		pr_err("ERROR, NO enough mem for %s!\n", str);
		length = 0;
		goto exit;
	}

	ret = of_property_read_u8_array(p_node, str, p, length);
	if (ret) {
		pr_err("no of property %s!\n", str);
		kfree(p);
		p = NULL;
		goto exit;
	}

	*lenp = length;

exit: return p;
}

static int of_get_eq_pdata(struct tas57xx_platform_data *pdata,
			   struct device_node *p_node)
{
	int length = 0;
	char *regs = NULL;
	int ret = 0;
	int num_eq;
	struct tas57xx_eq_cfg *eq_configs;

	ret = of_property_read_u32(p_node, "eq_enable", &pdata->eq_enable);
	if (pdata->eq_enable == 0 || ret != 0) {
		pr_err("Fail to get eq_enable node or EQ disable!\n");
		return -2;
	}

	num_eq = 4;
	pdata->num_eq_cfgs = num_eq;

	eq_configs = kcalloc(num_eq, sizeof(struct tas57xx_eq_cfg),
					GFP_KERNEL);

	if (eq_configs == NULL) {
		pr_err("ERROR, NO enough mem for eq_configs!\n");
		return -ENOMEM;
	}

	regs = alloc_and_get_data_array(p_node, table, &length);
	if (regs == NULL) {
		kfree(eq_configs);
		return -2;
	}
	strncpy(eq_configs[0].name, table, NAME_SIZE);
	eq_configs[0].regs = regs;
	eq_configs[0].reg_bytes = length;

	regs = alloc_and_get_data_array(p_node, woofer1, &length);
	if (regs == NULL) {
		pr_err("Fail to get eq_enable woofer1!!\n");
	} else {
		strncpy(eq_configs[1].name, woofer1, NAME_SIZE);
		eq_configs[1].regs = regs;
		eq_configs[1].reg_bytes = length;
	}

	regs = alloc_and_get_data_array(p_node, woofer2, &length);
	if (regs == NULL) {
		pr_err("Fail to get eq_enable woofer2!!\n");
	} else {
		strncpy(eq_configs[2].name, woofer2, NAME_SIZE);
		eq_configs[2].regs = regs;
		eq_configs[2].reg_bytes = length;
	}

	regs = alloc_and_get_data_array(p_node, null, &length);
	if (regs == NULL) {
		pr_err("Fail to get eq_enable null_0!!\n");
	} else {
		strncpy(eq_configs[3].name, null, NAME_SIZE);
		eq_configs[3].regs = regs;
		eq_configs[3].reg_bytes = length;
	}

	pdata->eq_cfgs = eq_configs;

	return 0;
}
static int of_get_drc_pdata(struct tas57xx_platform_data *pdata,
				struct device_node *p_node)
{
	int length = 0;
	char *pd = NULL;
	int ret = 0;

	ret = of_property_read_u32(p_node, "drc_enable", &pdata->drc_enable);
	if (pdata->drc_enable == 0 || ret != 0) {
		pr_err("Fail to get drc_enable node or DRC disable!\n");
		return -2;
	}

	/* get drc1 table */
	pd = alloc_and_get_data_array(p_node, drc1_table, &length);
	if (pd == NULL)
		return -2;
	pdata->custom_drc1_table_len = length;
	pdata->custom_drc1_table = pd;

	/* get drc1 tko table */
	length = 0;
	pd = NULL;

	pd = alloc_and_get_data_array(p_node, drc1_tko_table, &length);
	if (pd == NULL)
		return -2;
	pdata->custom_drc1_tko_table_len = length;
	pdata->custom_drc1_tko_table = pd;
	pdata->enable_ch1_drc = 1;

	/* get drc2 table */
	length = 0;
	pd = NULL;
	pd = alloc_and_get_data_array(p_node, drc2_table, &length);
	if (pd == NULL)
		return -1;
	pdata->custom_drc2_table_len = length;
	pdata->custom_drc2_table = pd;

	/* get drc2 tko table */
	length = 0;
	pd = NULL;
	pd = alloc_and_get_data_array(p_node, drc2_tko_table, &length);
	if (pd == NULL)
		return -1;
	pdata->custom_drc2_tko_table_len = length;
	pdata->custom_drc2_tko_table = pd;
	pdata->enable_ch2_drc = 1;

	return 0;
}

static int of_get_init_pdata(struct tas57xx_platform_data *pdata,
				 struct device_node *p_node)
{
	int length = 0;
	char *pd = NULL;

	pd = alloc_and_get_data_array(p_node, "input_mux_reg_buf", &length);
	if (pd == NULL) {
		pr_err("%s : can't get input_mux_reg_buf\n", __func__);
		pdata->num_init_regs = 0;
		pdata->init_regs = NULL;
	} else {
		pdata->num_init_regs = length;
		pdata->init_regs = pd;
	}

	if (of_property_read_u32(p_node, "master_vol",
				 &pdata->custom_master_vol)) {
		pr_err("%s fail to get master volume\n", __func__);
	}

	return 0;
}

static int of_get_resetpin_pdata(struct tas57xx_platform_data *pdata,
				 struct device_node *p_node)
{
	int ret;
	int reset_gpio;
	enum of_gpio_flags flags;

	reset_gpio = of_get_named_gpio_flags(p_node, "reset_pin", 0, &flags);
	if (reset_gpio < 0) {
		pr_err("%s fail to get reset pin from dts!\n", __func__);
		ret = -1;
	} else {
		int reset_pin = reset_gpio;

		ret = gpio_request(reset_pin, "codec_reset");
		pdata->reset_pin_active_low = flags & OF_GPIO_ACTIVE_LOW;

		if (pdata->reset_pin_active_low)
			gpio_direction_output(reset_pin, GPIOF_OUT_INIT_LOW);
		else
			gpio_direction_output(reset_pin, GPIOF_OUT_INIT_HIGH);

		pdata->reset_pin = reset_pin;
		pr_info("%s pdata->reset_pin = %d!\n", __func__,
			pdata->reset_pin);
	}
	return 0;
}

static int of_get_pdnpin_pdata(struct tas57xx_platform_data *pdata,
	struct device_node *p_node)
{
	int ret;
	int pdn_gpio;
	enum of_gpio_flags flags;

	pdn_gpio = of_get_named_gpio_flags(p_node, "pdn_pin", 0, &flags);
	if (pdn_gpio < 0) {
		pr_err("%s fail to get pdn pin from dts!\n", __func__);
		ret = -1;
	} else {
		int pdn_pin = pdn_gpio;

		gpio_request(pdn_pin, "codec_pdn");
		pdata->pdn_pin_active_low = flags & OF_GPIO_ACTIVE_LOW;

		if (pdata->pdn_pin_active_low)
			pr_info("%s is active low\n", __func__);
		else
			pr_info("%s is active high\n", __func__);

		if (pdata->pdn_pin_active_low)
			gpio_direction_output(pdn_pin, GPIOF_OUT_INIT_LOW);
		else
			gpio_direction_output(pdn_pin, GPIOF_OUT_INIT_HIGH);


		pdata->pdn_pin = pdn_pin;
		pr_info("%s pdata->pdn_pin = %d!\n", __func__,
			pdata->pdn_pin);
	}
	return 0;
}

static int of_get_phonepin_pdata(struct tas57xx_platform_data *pdata,
				 struct device_node *p_node)
{
	int ret;
	int phone_gpio;

	phone_gpio = of_get_named_gpio(p_node, "phone_pin", 0);
	if (phone_gpio < 0) {
		pr_err("%s fail to get phone pin from dts!\n", __func__);
		ret = -1;
	} else {
		gpio_request(phone_gpio, NULL);
		gpio_direction_output(phone_gpio, GPIOF_OUT_INIT_LOW);
		pdata->phone_pin = phone_gpio;
		pr_info("%s pdata->phone_pin = %d!\n", __func__,
			pdata->phone_pin);
	}
	return 0;
}

static int of_get_scanpin_pdata(struct tas57xx_platform_data *pdata,
				 struct device_node *p_node)
{
	int ret = 0;
	int scan_gpio;

	scan_gpio = of_get_named_gpio(p_node, "scan_pin", 0);
	if (scan_gpio < 0) {
		pr_err("%s fail to get scan pin from dts!\n", __func__);
		ret = -1;
	} else {
		gpio_request(scan_gpio, NULL);
		gpio_direction_input(scan_gpio);
		pdata->scan_pin = scan_gpio;
		pr_info("%s pdata->scan_pin = %d!\n", __func__,
			pdata->scan_pin);
	}
	return 0;
}

static int codec_get_of_pdata(struct tas57xx_platform_data *pdata,
				  struct device_node *p_node)
{
	int ret = 0;

	ret = of_get_resetpin_pdata(pdata, p_node);
	if (ret)
		pr_info("codec reset pin is not found in dts\n");
	ret = of_get_pdnpin_pdata(pdata, p_node);
	if (ret)
		pr_info("codec pdn pin is not found in dts\n");
	ret = of_get_phonepin_pdata(pdata, p_node);
	if (ret)
		pr_info("codec phone pin is not found in dtd\n");

	ret = of_get_scanpin_pdata(pdata, p_node);
	if (ret)
		pr_info("codec scanp pin is not found in dtd\n");

	ret = of_get_drc_pdata(pdata, p_node);
	if (ret == -2)
		pr_info("codec DRC configs are not found in dts\n");

	ret = of_get_eq_pdata(pdata, p_node);
	if (ret)
		pr_info("codec EQ configs are not found in dts\n");

	ret = of_get_init_pdata(pdata, p_node);
	if (ret)
		pr_info("codec init configs are not found in dts\n");
	return ret;
}


static int tas5731_i2c_probe(struct i2c_client *i2c,
				 const struct i2c_device_id *id)
{
	int ret;
	struct tas5731_priv *tas5731;
	struct tas57xx_platform_data *pdata;

	tas5731 = devm_kzalloc(&i2c->dev, sizeof(struct tas5731_priv),
				   GFP_KERNEL);

	if (!tas5731)
		return -ENOMEM;

	// Apply regmap
	tas5731->control_data = devm_regmap_init_i2c(i2c, &tas5731_regmap);
	if (IS_ERR(tas5731->control_data)) {
		ret = PTR_ERR(tas5731->control_data);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	pdata = devm_kzalloc(&i2c->dev, sizeof(struct tas57xx_platform_data),
				GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

  pdata->cls_spk = class_compat_register("fmspeaker");
  if (WARN(!pdata->cls_spk, "cannot allocate"))
			return -ENOMEM;

	tas5731->pdata = pdata;
	codec_get_of_pdata(pdata, i2c->dev.of_node);

	i2c_set_clientdata(i2c, tas5731);

	ret = snd_soc_register_codec(&i2c->dev, &tas5731_codec,
					 &tas5731_dai, 1);
	if (ret != 0)
		dev_err(&i2c->dev, "Failed to register codec (%d)\n", ret);

	return ret;
}

static int tas5731_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);

	return 0;
}

static void tas5731_i2c_shutdown(struct i2c_client *client)
{
	struct tas5731_priv *tas5731 = i2c_get_clientdata(client);
	struct tas57xx_platform_data *pdata = tas5731->pdata;

	dev_info(&client->dev, "Enter into tas5731 shutdown:\n");

	if (pdata->pdn_pin != 0 && pdata->reset_pin != 0) {
		/* before PowerDown:
		 * first we enter into shutdown state,
		 * so we need to wait 1ms + 1.3 x T(stop) following spec,
		 * about 165ms
		 */
		snd_soc_write(tas5731->codec, DDX_SYS_CTL_2, 0x40);
		mdelay(170);

		if (pdata->reset_pin_active_low)
			gpio_direction_output(pdata->reset_pin,
				GPIOF_OUT_INIT_LOW);
		else
			gpio_direction_output(pdata->reset_pin,
				GPIOF_OUT_INIT_HIGH);

		udelay(5);

		if (pdata->pdn_pin_active_low)
			gpio_direction_output(pdata->pdn_pin,
				GPIOF_OUT_INIT_LOW);
		else
			gpio_direction_output(pdata->pdn_pin,
				GPIOF_OUT_INIT_HIGH);

		dev_info(&client->dev, "Exit from shutdown, success!\n");
	} else
		dev_info(&client->dev, "Exit from shutdown, nothing to do\n");
}

static const struct i2c_device_id tas5731_i2c_id[] = {
	{ "tas5731", 0 },
	{}
};

static const struct of_device_id tas5731_of_id[] = {
	{.compatible = "ti,tas5731",},
	{}
};

static struct i2c_driver tas5731_i2c_driver = {
	.driver = {
		.name = "tas5731",
		.of_match_table = tas5731_of_id,
		.owner = THIS_MODULE,
	},
	.probe = tas5731_i2c_probe,
	.remove = tas5731_i2c_remove,
	.shutdown = tas5731_i2c_shutdown,
	.id_table = tas5731_i2c_id,
};

static int __init tas5731_modinit(void)
{
	int ret = 0;

	ret = i2c_add_driver(&tas5731_i2c_driver);
	if (ret < 0)
		pr_err("%s regiter i2x driver failed\n", __func__);

	return ret;
}
static void __exit tas5731_exit(void)
{
	i2c_del_driver(&tas5731_i2c_driver);
}

module_init(tas5731_modinit);
module_exit(tas5731_exit);

MODULE_DESCRIPTION("ASoC Tas5731 driver");
MODULE_AUTHOR("Fengmi & Amlogic");
MODULE_LICENSE("GPL");
