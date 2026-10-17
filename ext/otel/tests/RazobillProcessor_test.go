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
