#ifndef __OPLUS_MMS_GAUGE_H__
#define __OPLUS_MMS_GAUGE_H__

#include <oplus_mms.h>

#define GAUGE_INVALID_TEMP	(-400)

enum gauge_topic_item {
	GAUGE_ITEM_SOC,
	GAUGE_ITEM_VOL,
	GAUGE_ITEM_VOL_MAX,
	GAUGE_ITEM_VOL_MIN,
	GAUGE_ITEM_GAUGE_VBAT,
	GAUGE_ITEM_CURR,
	GAUGE_ITEM_TEMP,
	GAUGE_ITEM_FCC,
	GAUGE_ITEM_CC,
	GAUGE_ITEM_SOH,
	GAUGE_ITEM_RM,
	GAUGE_ITEM_BATT_EXIST,
	GAUGE_ITEM_ERR_CODE,
	GAUGE_ITEM_RESUME,
	GAUGE_ITEM_HMAC,
	GAUGE_ITEM_AUTH,
	GAUGE_ITEM_REAL_TEMP,
	GAUGE_ITEM_SUBBOARD_TEMP_ERR,
};

enum gauge_type_id {
	DEVICE_BQ27541,
	DEVICE_BQ27411,
	DEVICE_BQ28Z610,
	DEVICE_ZY0602,
	DEVICE_ZY0603,
};

int oplus_gauge_get_batt_mvolts_2(void);
int oplus_gauge_get_batt_fc_2(void);
int oplus_gauge_get_batt_qm_2(void);
int oplus_gauge_get_batt_pd_2(void);
int oplus_gauge_get_batt_rcu_2(void);
int oplus_gauge_get_batt_rcf_2(void);
int oplus_gauge_get_batt_fc_2u(void);
int oplus_gauge_get_batt_fc_2f(void);
int oplus_gauge_get_batt_sou_2(void);
int oplus_gauge_get_batt_do0_2(void);
int oplus_gauge_get_batt_doe_2(void);
int oplus_gauge_get_batt_trm_2(void);
int oplus_gauge_get_batt_pc_2(void);
int oplus_gauge_get_batt_qs_2(void);
int oplus_gauge_get_batt_mvolts_2_2cell_max(void);
int oplus_gauge_get_batt_mvolts_2_2cell_min(void);

int oplus_gauge_get_batt_soc_2(void);
int oplus_gauge_get_batt_current_2(void);
int oplus_gauge_get_remaining_capacity_2(void);
int oplus_gauge_get_device_type_2(void);
int oplus_gauge_get_device_type_2_for_vooc(void);

int oplus_gauge_get_batt_fc_2c(void);

int oplus_gauge_get_batt_cc_2(void);
int oplus_gauge_get_batt_soh_2(void);
bool oplus_gauge_get_batt_hmac_2(void);
bool oplus_gauge_get_batt_authenticate_2(void);
void oplus_gauge_set_batt_full_2(bool);
bool oplus_gauge_check_chip_is_null_2(void);
bool oplus_gauge_is_exist(struct oplus_mms *topic);

int oplus_gauge_update_battery_dod0_2(void);
int oplus_gauge_update_soc_smooth_parameter_2(void);
int oplus_gauge_get_battery_cb_status_2(void);
int oplus_gauge_get_i2c_err_2(void);
void oplus_gauge_clear_i2c_err_2(void);
int oplus_gauge_get_passedchg_2(int *val);
int oplus_gauge_dump_register_2(void);
int oplus_gauge_lock(void);
int oplus_gauge_unlock(void);
bool oplus_gauge_is_locked(void);
int oplus_gauge_get_batt_num(void);
int oplus_gauge_get_batt_capacity_mah(struct oplus_mms *topic);

int oplus_gauge_get_dod0(struct oplus_mms *mms, int index, int *val);
int oplus_gauge_get_dod0_passed_q(struct oplus_mms *mms, int index, int *val);
int oplus_gauge_get_qmax(struct oplus_mms *mms, int index, int *val);
int oplus_gauge_get_qmax_passed_q(struct oplus_mms *mms, int index, int *val);
int oplus_gauge_get_volt(struct oplus_mms *mms, int index, int *val);
int oplus_gauge_get_gauge_type(struct oplus_mms *mms, int index, int *val);
int oplus_gauge_get_bcc_parameters_2(char *buf);
int oplus_gauge_fastchg_update_bcc_parameters_2(char *buf);
int oplus_gauge_get_prev_bcc_parameters_2(char *buf);
int oplus_gauge_set_bcc_parameters_2(const char *buf);

int oplus_gauge_protect_check_2(void);
bool oplus_gauge_afi_update_done_2(void);

bool oplus_gauge_check_reset_condition(void);
bool oplus_gauge_reset(void);
bool is_support_parallel_battery(struct oplus_mms *topic);
#endif /* __OPLUS_MMS_GAUGE_H__ */
