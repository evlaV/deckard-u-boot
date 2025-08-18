
#include <linux/types.h>

/**
 * struct clk_regmap - regmap supporting clock
 * @enable_reg: register when using regmap enable/disable ops
 * @enable_mask: mask when using regmap enable/disable ops
 * @enable_is_inverted: flag to indicate set enable_mask bits to disable
 *                      when using clock_enable_regmap and friends APIs.
 */
struct clk_regmap {
	unsigned int enable_reg;
	unsigned int enable_mask;
	bool enable_is_inverted;
};

/* Alpha PLL types */
enum {
	CLK_ALPHA_PLL_TYPE_DEFAULT,
	CLK_ALPHA_PLL_TYPE_HUAYRA,
	CLK_ALPHA_PLL_TYPE_HUAYRA_APSS,
	CLK_ALPHA_PLL_TYPE_HUAYRA_2290,
	CLK_ALPHA_PLL_TYPE_BRAMMO,
	CLK_ALPHA_PLL_TYPE_FABIA,
	CLK_ALPHA_PLL_TYPE_TRION,
	CLK_ALPHA_PLL_TYPE_LUCID = CLK_ALPHA_PLL_TYPE_TRION,
	CLK_ALPHA_PLL_TYPE_AGERA,
	CLK_ALPHA_PLL_TYPE_ZONDA,
	CLK_ALPHA_PLL_TYPE_REGERA = CLK_ALPHA_PLL_TYPE_ZONDA,
	CLK_ALPHA_PLL_TYPE_ZONDA_OLE,
	CLK_ALPHA_PLL_TYPE_LUCID_EVO,
	CLK_ALPHA_PLL_TYPE_LUCID_OLE,
	CLK_ALPHA_PLL_TYPE_PONGO_ELU,
	CLK_ALPHA_PLL_TYPE_TAYCAN_ELU,
	CLK_ALPHA_PLL_TYPE_RIVIAN_EVO,
	CLK_ALPHA_PLL_TYPE_DEFAULT_EVO,
	CLK_ALPHA_PLL_TYPE_BRAMMO_EVO,
	CLK_ALPHA_PLL_TYPE_STROMER,
	CLK_ALPHA_PLL_TYPE_STROMER_PLUS,
	CLK_ALPHA_PLL_TYPE_NSS_HUAYRA,
	CLK_ALPHA_PLL_TYPE_MAX,
};

enum {
	PLL_OFF_L_VAL,
	PLL_OFF_CAL_L_VAL,
	PLL_OFF_ALPHA_VAL,
	PLL_OFF_ALPHA_VAL_U,
	PLL_OFF_USER_CTL,
	PLL_OFF_USER_CTL_U,
	PLL_OFF_USER_CTL_U1,
	PLL_OFF_CONFIG_CTL,
	PLL_OFF_CONFIG_CTL_U,
	PLL_OFF_CONFIG_CTL_U1,
	PLL_OFF_CONFIG_CTL_U2,
	PLL_OFF_TEST_CTL,
	PLL_OFF_TEST_CTL_U,
	PLL_OFF_TEST_CTL_U1,
	PLL_OFF_TEST_CTL_U2,
	PLL_OFF_TEST_CTL_U3,
	PLL_OFF_STATE,
	PLL_OFF_STATUS,
	PLL_OFF_OPMODE,
	PLL_OFF_FRAC,
	PLL_OFF_CAL_VAL,
	PLL_OFF_MAX_REGS
};

extern const u8 clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_MAX][PLL_OFF_MAX_REGS];

struct pll_vco {
	unsigned long min_freq;
	unsigned long max_freq;
	u32 val;
};

#define VCO(a, b, c) { \
	.val = a,\
	.min_freq = b,\
	.max_freq = c,\
}

/**
 * struct clk_alpha_pll - phase locked loop (PLL)
 * @offset: base address of registers
 * @regs: alpha pll register map (see @clk_alpha_pll_regs)
 * @vco_table: array of VCO settings
 * @num_vco: number of VCO settings in @vco_table
 * @flags: bitmask to indicate features supported by the hardware
 * @clkr: regmap clock handle
 */
struct clk_alpha_pll {
	phys_addr_t base;
	u32 offset;
	const u8 *regs;

	const char *name;

	const struct pll_vco *vco_table;
	size_t num_vco;
#define SUPPORTS_OFFLINE_REQ		BIT(0)
#define SUPPORTS_FSM_MODE		BIT(2)
#define SUPPORTS_DYNAMIC_UPDATE	BIT(3)
#define SUPPORTS_FSM_LEGACY_MODE	BIT(4)
	u8 flags;
};

struct alpha_pll_config {
	u32 l;
	u32 alpha;
	u32 alpha_hi;
	u32 config_ctl_val;
	u32 config_ctl_hi_val;
	u32 config_ctl_hi1_val;
	u32 config_ctl_hi2_val;
	u32 user_ctl_val;
	u32 user_ctl_hi_val;
	u32 user_ctl_hi1_val;
	u32 test_ctl_val;
	u32 test_ctl_mask;
	u32 test_ctl_hi_val;
	u32 test_ctl_hi_mask;
	u32 test_ctl_hi1_val;
	u32 test_ctl_hi2_val;
	u32 test_ctl_hi3_val;
	u32 main_output_mask;
	u32 aux_output_mask;
	u32 aux2_output_mask;
	u32 early_output_mask;
	u32 alpha_en_mask;
	u32 alpha_mode_mask;
	u32 pre_div_val;
	u32 pre_div_mask;
	u32 post_div_val;
	u32 post_div_mask;
	u32 vco_val;
	u32 vco_mask;
	u32 status_val;
	u32 status_mask;
	u32 lock_det;
};

int alpha_pll_lucid_evo_enable(struct clk_alpha_pll *pll);
void clk_lucid_ole_pll_configure(struct clk_alpha_pll *pll,
				 const struct alpha_pll_config *config);

// prate (parent rate) is typically BI_TCXO, something like 38400000
int alpha_trion_pll_set_rate(struct clk_alpha_pll *pll, unsigned long rate,
				    unsigned long prate);
