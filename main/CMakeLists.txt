# main 组件不写 REQUIRES 时默认依赖全部组件，cJSON（json 组件）、
# driver/uart、esp_wifi、mqtt_client 都能直接用。
# 一旦显式写了 REQUIRES/PRIV_REQUIRES，就只剩列出的那些，容易漏。
idf_component_register(SRCS "inverter_collector.c" "modbus.c" "modbus_selftest.c" "wifi.c" "udp.c" "tcp.c" "mqtt_report.c" "mqtt_cmd.c"
                    INCLUDE_DIRS ".")
