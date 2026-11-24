# main 组件不写 REQUIRES 时默认依赖全部组件，cJSON（json 组件）、
# driver/uart、esp_wifi、mqtt_client、protobuf-c 都能直接用。
# 一旦显式写了 REQUIRES/PRIV_REQUIRES，就只剩列出的那些，容易漏。
#
# telemetry.pb-c.c 由 proto/telemetry.proto 生成，生成命令见 README。
# 生成文件要提交到仓库，避免他人拉代码后因缺 protoc-c 无法编译。
#
# telemetry_codec.c 是编解码封装层，上层只调 codec_* 接口，不直接碰 pb-c.h，
# 避免忘记 free_unpacked、或用 strlen 量二进制长度。
idf_component_register(SRCS "inverter_collector.c" "modbus.c" "modbus_selftest.c" "wifi.c" "udp.c" "tcp.c" "mqtt_report.c" "mqtt_cmd.c" "telemetry.pb-c.c" "telemetry_codec.c"
                    INCLUDE_DIRS ".")
