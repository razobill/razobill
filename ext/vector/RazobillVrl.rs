// =============================================================================
// Vector Remap Language (VRL) native transform for razobill
//
// wraps librazobill Universal C ABI in Rust; exposes parse_razobill function
// decomposing log messages into structured JSON-compatible event maps
// =============================================================================

//! native Vector Remap Language (VRL) transform wrapping librazobill C ABI

use std::collections::BTreeMap;
use std::os::raw::c_char;

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

/// C ABI status codes matching RazobillC.h RzStatus
#[repr(C)]
#[derive(Debug, PartialEq, Eq)]
pub enum RzStatus {
    Ok = 0,
    ErrInvalidArgument = 1,
    ErrBufferTooSmall = 2,
    ErrMalformedTemplate = 3,
    ErrCountMismatch = 4,
    ErrMessageTooLarge = 5,
    ErrOutOfMemory = 6,
    ErrInternal = 7,
}

extern "C" {
    pub fn RzSplit(
        message: *const c_char,
        message_length: usize,
        out_record: *mut RzRecord,
    ) -> RzStatus;

    pub fn RzFree(record: *mut RzRecord);
}

/// decompose raw log message into structured JSON event map
/// inv: template and parameter slices reconstruct original message
/// in: raw_msg is UTF-8 string slice
/// out: Ok(BTreeMap) with "template", "form", "ints", and "strs"
/// own: returns owned BTreeMap and serde_json::Value objects
/// thread: thread-safe, no shared state
/// err: returns Err with RzStatus description if RzSplit fails
pub fn parse_razobill(raw_msg: &str) -> Result<BTreeMap<String, serde_json::Value>, String> {
    let mut record: RzRecord = unsafe { std::mem::zeroed() };

    let status = unsafe {
        RzSplit(
            raw_msg.as_ptr() as *const c_char,
            raw_msg.len(),
            &mut record,
        )
    };

    if !matches!(status, RzStatus::Ok) {
        return Err(format!(
            "Razobill decomposition failed with status {status:?}"
        ));
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

    // Vector VRL represents internal event values as UTF-8 JSON objects
    // (serde_json::Value); using from_utf8_lossy prevents malformed byte
    // sequences from aborting the pipeline, replacing bad bytes with U+FFFD
    let form_str = if record.template_length > 0 && !record.template_str.is_null() {
        let form_slice = unsafe {
            std::slice::from_raw_parts(record.template_str as *const u8, record.template_length)
        };
        String::from_utf8_lossy(form_slice).to_string()
    } else {
        String::new()
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
                strs.push(serde_json::Value::from(
                    String::from_utf8_lossy(slice).to_string(),
                ));
            } else {
                strs.push(serde_json::Value::from(""));
            }
        }
    }

    // build structured event map with both "template" and "form" keys
    let mut map = BTreeMap::new();
    map.insert(
        "template".to_string(),
        serde_json::Value::from(form_str.clone()),
    );
    map.insert("form".to_string(), serde_json::Value::from(form_str));
    map.insert("ints".to_string(), serde_json::Value::Array(ints));
    map.insert("strs".to_string(), serde_json::Value::Array(strs));

    Ok(map)
}
