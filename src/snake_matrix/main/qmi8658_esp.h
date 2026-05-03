#pragma once

#include "esp_err.h"

esp_err_t qmi8658_esp_init(void);

/** Acceleration in g (same convention as Arduino SensorLib demo). */
esp_err_t qmi8658_esp_read_accel(float *ax_g, float *ay_g, float *az_g);
