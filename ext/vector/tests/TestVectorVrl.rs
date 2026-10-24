// =============================================================================
// Vector Remap Language (VRL) integration test suite
//
// verifies native Rust bindings against sample sshd authentication records
// =============================================================================

use razobill_vector::parse_razobill;

/// pins VRL event map decomposition guarantees for mixed int and string fields
/// ~ if sshd changes its port formatting, every SIEM in existence breaks
#[test]
fn test_parse_razobill() {
    // standard openssh auth line with ip address, port, and username
    let msg = "Accepted publickey for ubuntu from 192.168.1.100 port 54322 ssh2";
    let res = parse_razobill(msg).expect("Failed to parse log record");

    // verify core map keys populated by native transform
    assert!(res.contains_key("template"));
    assert!(res.contains_key("form"));
    assert_eq!(res.get("template"), res.get("form"));
    assert!(res.contains_key("ints"));
    assert!(res.contains_key("strs"));

    // extracted port integer
    let ints = res.get("ints").unwrap().as_array().unwrap();
    assert_eq!(ints.len(), 1);
    assert_eq!(ints[0].as_i64().unwrap(), 54322);

    // extracted variable strings: username, ip address, protocol version
    let strs = res.get("strs").unwrap().as_array().unwrap();
    assert_eq!(strs.len(), 3);
    assert_eq!(strs[0].as_str().unwrap(), "ubuntu");
    assert_eq!(strs[1].as_str().unwrap(), "192.168.1.100");
    assert_eq!(strs[2].as_str().unwrap(), "ssh2");
}

#[test]
fn test_parse_razobill_raw_bytes() {
    let raw_bytes: &[u8] = b"Worker 100 finished in 250ms";
    let res = parse_razobill(raw_bytes).expect("Failed to parse byte slice");

    assert_eq!(
        res.get("template").unwrap().as_str().unwrap(),
        "Worker {i} finished in {s}"
    );
    let ints = res.get("ints").unwrap().as_array().unwrap();
    assert_eq!(ints[0].as_i64().unwrap(), 100);
    let strs = res.get("strs").unwrap().as_array().unwrap();
    assert_eq!(strs[0].as_str().unwrap(), "250ms");
}

#[test]
fn test_parse_razobill_non_utf8_lossless() {
    let mut bad_bytes = Vec::new();
    bad_bytes.extend_from_slice(b"invalid \xFF\xFE user 42");
    let res = parse_razobill(&bad_bytes).expect("Failed to parse non-UTF8 slice");

    assert!(res.contains_key("template"));
    assert!(res.contains_key("ints"));
    assert!(res.contains_key("strs"));
    let ints = res.get("ints").unwrap().as_array().unwrap();
    assert_eq!(ints[0].as_i64().unwrap(), 42);
}

#[test]
fn test_parse_razobill_empty_input() {
    let res = parse_razobill("").expect("Failed to parse empty string");
    assert_eq!(res.get("template").unwrap().as_str().unwrap(), "");
    assert_eq!(res.get("ints").unwrap().as_array().unwrap().len(), 0);
    assert_eq!(res.get("strs").unwrap().as_array().unwrap().len(), 0);

    let res_bytes = parse_razobill(b"").expect("Failed to parse empty bytes");
    assert_eq!(res_bytes.get("template").unwrap().as_str().unwrap(), "");
    assert_eq!(res_bytes.get("ints").unwrap().as_array().unwrap().len(), 0);
    assert_eq!(res_bytes.get("strs").unwrap().as_array().unwrap().len(), 0);
}

#[test]
fn test_parse_razobill_no_slots() {
    let msg = "Static log line without dynamic slots.";
    let res = parse_razobill(msg).expect("Failed to parse static line");
    assert_eq!(res.get("template").unwrap().as_str().unwrap(), msg);
    assert_eq!(res.get("ints").unwrap().as_array().unwrap().len(), 0);
    assert_eq!(res.get("strs").unwrap().as_array().unwrap().len(), 0);
}
