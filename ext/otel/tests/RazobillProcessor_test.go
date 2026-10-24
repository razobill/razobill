// =============================================================================
// OpenTelemetry Collector processor integration tests for razobill
//
// verifies end-to-end log record consumption, C ABI decomposition, body
// replacement, and attribute population (ints, strs, formHash)
// =============================================================================

package tests

import (
	"context"
	"testing"

	rz "github.com/razobill/razobill/ext/otel"
	"go.opentelemetry.io/collector/consumer/consumertest"
	"go.opentelemetry.io/collector/pdata/plog"
)

// TestRazobillProcessor_ConsumeLogs pins end-to-end log record decomposition
func TestRazobillProcessor_ConsumeLogs(t *testing.T) {
	// pins log record decomposition and attribute attachment in OTel pipeline
	sink := new(consumertest.LogsSink)
	cfg := rz.CreateDefaultConfig()
	cfg.DropRawBody = true
	cfg.IncludeHash = true

	proc, err := rz.NewRazobillProcessor(cfg, sink)
	if err != nil {
		t.Fatalf("Failed to initialize razobill processor: %v", err)
	}

	ld := plog.NewLogs()
	rl := ld.ResourceLogs().AppendEmpty()
	sl := rl.ScopeLogs().AppendEmpty()
	rec := sl.LogRecords().AppendEmpty()
	rec.Body().SetStr("Accepted publickey for ubuntu from 192.168.1.100 port 54322 ssh2")

	err = proc.ConsumeLogs(context.Background(), ld)
	if err != nil {
		t.Fatalf("ConsumeLogs returned error: %v", err)
	}

	outLogs := sink.AllLogs()
	if len(outLogs) != 1 {
		t.Fatalf("Expected 1 logs batch, got %d", len(outLogs))
	}

	outRec := outLogs[0].ResourceLogs().At(0).ScopeLogs().At(0).LogRecords().At(0)
	attrs := outRec.Attributes()

	if _, ok := attrs.Get("razobill.formHash"); !ok {
		t.Errorf("Expected attribute razobill.formHash to be set")
	}

	intsVal, ok := attrs.Get("razobill.ints")
	if !ok || intsVal.Slice().Len() != 1 || intsVal.Slice().At(0).Int() != 54322 {
		t.Errorf("Expected razobill.ints to contain [54322], got %v", intsVal)
	}

	strsVal, ok := attrs.Get("razobill.strs")
	if !ok || strsVal.Slice().Len() != 3 {
		t.Errorf("Expected razobill.strs to contain 3 elements, got %v", strsVal)
	}
}

// TestRazobillProcessor_ConsumeLogs_Bytes verifies byte-type log record decomposition
func TestRazobillProcessor_ConsumeLogs_Bytes(t *testing.T) {
	sink := new(consumertest.LogsSink)
	cfg := rz.CreateDefaultConfig()
	cfg.DropRawBody = true
	cfg.IncludeHash = true

	proc, err := rz.NewRazobillProcessor(cfg, sink)
	if err != nil {
		t.Fatalf("Failed to initialize razobill processor: %v", err)
	}

	ld := plog.NewLogs()
	rl := ld.ResourceLogs().AppendEmpty()
	sl := rl.ScopeLogs().AppendEmpty()
	rec := sl.LogRecords().AppendEmpty()
	raw := []byte("Worker 42 processed task 99")
	rec.Body().SetEmptyBytes().FromRaw(raw)

	err = proc.ConsumeLogs(context.Background(), ld)
	if err != nil {
		t.Fatalf("ConsumeLogs returned error: %v", err)
	}

	outLogs := sink.AllLogs()
	if len(outLogs) != 1 {
		t.Fatalf("Expected 1 logs batch, got %d", len(outLogs))
	}

	outRec := outLogs[0].ResourceLogs().At(0).ScopeLogs().At(0).LogRecords().At(0)
	attrs := outRec.Attributes()

	intsVal, ok := attrs.Get("razobill.ints")
	if !ok || intsVal.Slice().Len() != 2 {
		t.Fatalf("Expected razobill.ints to contain 2 elements, got %v", intsVal)
	}
	if intsVal.Slice().At(0).Int() != 42 || intsVal.Slice().At(1).Int() != 99 {
		t.Errorf("Expected [42, 99], got [%d, %d]", intsVal.Slice().At(0).Int(), intsVal.Slice().At(1).Int())
	}

	// Since DropRawBody is true, body should now be the template as bytes
	expectedTmpl := "Worker {i} processed task {i}"
	if string(outRec.Body().Bytes().AsRaw()) != expectedTmpl {
		t.Errorf("Expected body %q, got %q", expectedTmpl, string(outRec.Body().Bytes().AsRaw()))
	}
}

// TestConfig_Validate verifies configuration validation enforces semantic mode
func TestConfig_Validate(t *testing.T) {
	cfg := rz.CreateDefaultConfig()
	if err := cfg.Validate(); err != nil {
		t.Errorf("Default config failed validation: %v", err)
	}

	cfg.Mode = "exact"
	if err := cfg.Validate(); err == nil {
		t.Errorf("Expected error for exact mode, got nil")
	}

	cfg.Mode = "invalid_mode"
	if err := cfg.Validate(); err == nil {
		t.Errorf("Expected error for invalid mode, got nil")
	}
}
