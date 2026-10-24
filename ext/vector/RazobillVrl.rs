// =============================================================================
// Vector Remap Language (VRL) native transform for razobill
//
// wraps librazobill Universal C ABI in Rust; exposes parse_razobill function
// decomposing log messages into structured JSON-compatible event maps
// =============================================================================

//! native Vector Remap Language (VRL) transform wrapping librazobill C ABI

use std::collections::BTreeMap;
use std::os::raw::{c_char, c_int};

/// C ABI record structure returned by RzSplit
/// own: internal memory allocated by C engine; must be released via RzFree
#[repr(C)]
#[derive(Debug)]
pub struct RzRecord {
    pub template_str: *const c_char,
    pub template_length: usize,
    pub int_values: *const i64,
    pub int_count: usize,
    pub str_values: *const *const c_char,
    pub str_lengths: *const usize,
    pub str_count: usize,
    pub internal_handle: *mut std::ffi::c_void,
}

/// C ABI status code type matching RazobillC.h RzStatus
pub type RzStatus = c_int;
pub const RZ_OK: RzStatus = 0;
pub const RZ_ERR_INVALID_ARGUMENT: RzStatus = 1;
pub const RZ_ERR_BUFFER_TOO_SMALL: RzStatus = 2;
pub const RZ_ERR_MALFORMED_TEMPLATE: RzStatus = 3;
pub const RZ_ERR_COUNT_MISMATCH: RzStatus = 4;
pub const RZ_ERR_MESSAGE_TOO_LARGE: RzStatus = 5;
pub const RZ_ERR_OUT_OF_MEMORY: RzStatus = 6;
pub const RZ_ERR_INTERNAL: RzStatus = 7;

extern "C" {
    pub fn RzSplit(
        message: *const c_char,
        message_length: usize,
        out_record: *mut RzRecord,
    ) -> RzStatus;

    pub fn RzFree(record: *mut RzRecord);

    pub fn RzStatusMessage(status: RzStatus) -> *const c_char;
}

/// decompose raw log message into structured JSON event map
/// inv: template and parameter slices reconstruct original message
/// in: raw_msg is UTF-8 string or raw byte slice
/// out: Ok(BTreeMap) with "template", "form" (alias), "ints", and "strs"
/// own: returns owned BTreeMap and serde_json::Value objects
/// thread: thread-safe, no shared state
/// err: returns Err with RzStatusMessage description if RzSplit fails
pub fn parse_razobill(
    raw_msg: impl AsRef<[u8]>,
) -> Result<BTreeMap<String, serde_json::Value>, String> {
    let raw_bytes = raw_msg.as_ref();
    let mut record: RzRecord = unsafe { std::mem::zeroed() };

    let status = unsafe {
        RzSplit(
            raw_bytes.as_ptr() as *const c_char,
            raw_bytes.len(),
            &mut record,
        )
    };

    if status != RZ_OK {
        let msg_ptr = unsafe { RzStatusMessage(status) };
        let desc = if !msg_ptr.is_null() {
            unsafe { std::ffi::CStr::from_ptr(msg_ptr) }
                .to_string_lossy()
                .into_owned()
        } else {
            format!("status code {status}")
        };
        return Err(format!("Razobill decomposition failed: {desc}"));
    }

    // RAII guard ensuring RzFree is invoked on return or unwind
    struct RecordGuard(*mut RzRecord);
    impl Drop for RecordGuard {
        fn drop(&mut self) {
            unsafe {
                RzFree(self.0);
            }
        }
    }
    let _guard = RecordGuard(&mut record as *mut RzRecord);

    let template_val = if record.template_length > 0 && !record.template_str.is_null() {
        let slice = unsafe {
            std::slice::from_raw_parts(record.template_str as *const u8, record.template_length)
        };
        match std::str::from_utf8(slice) {
            Ok(s) => serde_json::Value::from(s),
            Err(_) => serde_json::Value::from(slice.to_vec()),
        }
    } else {
        serde_json::Value::from("")
    };

    let mut ints = Vec::with_capacity(record.int_count);
    if record.int_count > 0 && !record.int_values.is_null() {
        let int_slice = unsafe { std::slice::from_raw_parts(record.int_values, record.int_count) };
        for &val in int_slice {
            ints.push(serde_json::Value::from(val));
        }
    }

    let mut strs = Vec::with_capacity(record.str_count);
    if record.str_count > 0 && !record.str_values.is_null() && !record.str_lengths.is_null() {
        let str_ptrs = unsafe { std::slice::from_raw_parts(record.str_values, record.str_count) };
        let str_lens = unsafe { std::slice::from_raw_parts(record.str_lengths, record.str_count) };
        for i in 0..record.str_count {
            if str_lens[i] > 0 && !str_ptrs[i].is_null() {
                let slice =
                    unsafe { std::slice::from_raw_parts(str_ptrs[i] as *const u8, str_lens[i]) };
                match std::str::from_utf8(slice) {
                    Ok(s) => strs.push(serde_json::Value::from(s)),
                    Err(_) => strs.push(serde_json::Value::from(slice.to_vec())),
                }
            } else {
                strs.push(serde_json::Value::from(""));
            }
        }
    }

    // build structured event map
    let mut map = BTreeMap::new();
    map.insert("template".to_string(), template_val.clone());
    // "form" is retained as a documented legacy backward-compatible alias for "template"
    map.insert("form".to_string(), template_val);
    map.insert("ints".to_string(), serde_json::Value::Array(ints));
    map.insert("strs".to_string(), serde_json::Value::Array(strs));

    Ok(map)
}
