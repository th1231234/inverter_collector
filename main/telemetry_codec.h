#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * Protobuf 编解码封装（阶段三）
 * ------------------------------------------------------------------
 * 设计意图：上层（mqtt_report / mqtt_cmd）不直接接触 telemetry.pb-c.h，
 * 也就不会误用 ProtobufCMessage、忘记 free_unpacked、或把二进制长度
 * 交给 strlen。所有 protobuf 相关的内存与长度都收在这个文件里。
 *
 * 内存约定：
 *   所有 codec_encode_* 成功时把 malloc 出来的缓冲交给调用方，
 *   调用方用完后必须 codec_free()。失败时 *out 保持 NULL。
 * ================================================================== */

/* req_id 上限，与 mqtt_cmd 的 CMD_MAX_REQ_ID 保持一致 */
#define CODEC_MAX_REQ_ID     32

/* 单次读取寄存器数量上限 */
#define CODEC_MAX_READ_NUM   16

/* 单帧上报的测点总数上限。
 * 多从机是"所有从机的所有测点拍平成一帧"，所以总数不再等于单台的 3 个：
 * 3 个从机 × 16 个测点 = 48。真正的硬边界仍是 CODEC_MAX_PAYLOAD，
 * 这个常量只是提前挡住明显写错的调用（比如把 n_metrics 传成字节数）。 */
#define CODEC_MAX_METRICS    48

/* 下行报文长度上限。超过直接拒绝，避免 unpack 解析超长输入 */
#define CODEC_MAX_PAYLOAD    512

/* ------------------------------------------------------------------
 * 解析后的请求（不暴露 protobuf 类型）
 * ------------------------------------------------------------------ */
typedef struct {
    char     req_id[CODEC_MAX_REQ_ID];
    uint8_t  slave;
    uint16_t addr;
    uint16_t num;      /* 仅 read */
} codec_read_req_t;

typedef struct {
    char     req_id[CODEC_MAX_REQ_ID];
    uint8_t  slave;
    uint16_t addr;
    uint16_t value;    /* 仅 write */
} codec_write_req_t;

/* 解码失败的原因，便于日志区分 */
typedef enum {
    CODEC_OK = 0,
    CODEC_ERR_TOO_LONG,    /* 长度超上限 */
    CODEC_ERR_UNPACK,      /* unpack 返回 NULL：数据非法或截断 */
    CODEC_ERR_REQ_ID,      /* req_id 缺失或字符非法 */
    CODEC_ERR_RANGE,       /* slave/addr/num/value 越界 */
} codec_result_t;

/* ------------------------------------------------------------------
 * 一个测点（对应 proto 里的 Metric）
 *
 * 用结构体数组而不用四个平行数组传参：站号、地址、原始值、物理值本来就
 * 属于同一个测点，拆成平行数组后调用方很容易把顺序写错，编译器也查不出来。
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t  slave;    /* 从机站号 1~247，多从机场景用于区分归属 */
    uint16_t addr;     /* 寄存器地址，如 0x3000 */
    uint16_t raw;      /* 从机原始值，16 位整数 */
    float    value;    /* 换算后的物理值，如 350.0 */
} codec_metric_t;

/* ------------------------------------------------------------------
 * 时间基准（SNTP）
 *
 * Telemetry.ts_ms 用 Unix 毫秒。未完成对时前退化为开机毫秒，
 * 保证字段永远有值（上位机可据此判断"没有真实时间"）。
 * ------------------------------------------------------------------ */
void    codec_time_start(void);       /* 启动 SNTP，异步，不阻塞 */
bool    codec_time_is_synced(void);
int64_t codec_now_ms(void);

/* ------------------------------------------------------------------
 * 编码：遥测帧
 *
 * @param metrics    测点数组，长度 n_metrics
 * @param out/out_len 成功时返回 malloc 的缓冲与真实字节数
 *
 * 注意 out_len 是 protobuf 的真实长度，不是 strlen ——
 * 报文里可能出现 0x00（例如 raw=0），按 C 字符串处理会被截断。
 * ------------------------------------------------------------------ */
bool codec_encode_telemetry(uint32_t seq, int32_t state,
                            const codec_metric_t *metrics, int n_metrics,
                            uint8_t **out, size_t *out_len);

/* ------------------------------------------------------------------
 * 解码：下行请求
 *
 * 内部依次校验：长度上限 → unpack 判空 → req_id 合法性 → 各字段范围。
 * 任何一步失败都返回对应错误码，且 *out 不被修改。
 * ------------------------------------------------------------------ */
codec_result_t codec_decode_read(const uint8_t *data, size_t len, codec_read_req_t *out);
codec_result_t codec_decode_write(const uint8_t *data, size_t len, codec_write_req_t *out);

/* 把错误码转成可读字符串，只用于日志 */
const char *codec_result_name(codec_result_t r);

/* ------------------------------------------------------------------
 * 编码：应答
 * ------------------------------------------------------------------ */
bool codec_encode_resp_ok(const char *req_id, uint8_t **out, size_t *out_len);

bool codec_encode_resp_values(const char *req_id,
                              const uint16_t *vals, int n,
                              uint8_t **out, size_t *out_len);

bool codec_encode_resp_error(const char *req_id, const char *msg,
                             uint8_t **out, size_t *out_len);

/* 释放 codec_encode_* 返回的缓冲 */
void codec_free(uint8_t *buf);

#ifdef __cplusplus
}
#endif
