// =============================================================================
// OpenTelemetry Collector log processor implementation for razobill
//
// decomposes incoming log record bodies into structural templates, integers,
// and variable strings via the Razobill C ABI, attaching them as attributes
// =============================================================================

package razobillprocessor

/*
#cgo CFLAGS: -I../../include
#cgo LDFLAGS: -L../../build -lrazobill_cabi
#cgo linux LDFLAGS: -lstdc++
#include "librazobill/RazobillC.h"
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"fmt"
	"unsafe"

	"github.com/cespare/xxhash/v2"
	"go.opentelemetry.io/collector/component"
	"go.opentelemetry.io/collector/consumer"
	"go.opentelemetry.io/collector/pdata/pcommon"
	"go.opentelemetry.io/collector/pdata/plog"
)

// RazobillProcessor intercepts OpenTelemetry log records and decomposes bodies
type RazobillProcessor struct {
	cfg          *Config
	nextConsumer consumer.Logs
}

// NewRazobillProcessor creates an OpenTelemetry log record processor
// in: cfg is validated configuration; next is downstream consumer
// out: initialized *RazobillProcessor ready for lifecycle Start
// err: nil under current implementation
func NewRazobillProcessor(cfg *Config, next consumer.Logs) (*RazobillProcessor, error) {
	return &RazobillProcessor{
		cfg:          cfg,
		nextConsumer: next,
	}, nil
}

func newRazobillProcessor(cfg *Config, next consumer.Logs) (*RazobillProcessor, error) {
	return NewRazobillProcessor(cfg, next)
}

// Start initializes processor resources during collector startup
func (p *RazobillProcessor) Start(_ context.Context, _ component.Host) error {
	if int(C.RzAbiVersion()) != 1 {
		return fmt.Errorf("incompatible librazobill ABI version: got %d, want 1", int(C.RzAbiVersion()))
	}
	return nil
}

// Shutdown flushes and tears down processor resources during collector shutdown
func (p *RazobillProcessor) Shutdown(_ context.Context) error {
	return nil
}

// Capabilities signals that this processor mutates log record bodies/attributes
func (p *RazobillProcessor) Capabilities() consumer.Capabilities {
	return consumer.Capabilities{MutatesData: true}
}

// processRecord decomposes a single log record body via the C ABI with guaranteed cleanup
func (p *RazobillProcessor) processRecord(rec plog.LogRecord) {
	var rawBytes []byte
	isBytes := false

	switch rec.Body().Type() {
	case pcommon.ValueTypeStr:
		strVal := rec.Body().Str()
		rawBytes = []byte(strVal)
	case pcommon.ValueTypeBytes:
		rawBytes = rec.Body().Bytes().AsRaw()
		isBytes = true
	default:
		strVal := rec.Body().AsString()
		rawBytes = []byte(strVal)
	}

	if len(rawBytes) == 0 {
		if p.cfg.DropRawBody {
			if isBytes {
				rec.Body().SetEmptyBytes().FromRaw([]byte{})
			} else {
				rec.Body().SetStr("")
			}
		} else {
			rec.Attributes().PutStr("razobill.template", "")
		}
		if p.cfg.IncludeHash {
			rec.Attributes().PutInt("razobill.formHash", int64(xxhash.Sum64String("")))
		}
		return
	}

	cMsg := C.CBytes(rawBytes)
	defer C.free(unsafe.Pointer(cMsg))

	var cRecord C.RzRecord
	defer C.RzFree(&cRecord)

	// invoke librazobill C ABI to parse message into template and slots
	status := C.RzSplit(
		(*C.char)(cMsg),
		C.size_t(len(rawBytes)),
		&cRecord,
	)

	if status == C.RZ_OK {
		tmplStr := C.GoStringN(cRecord.templateStr, C.int(cRecord.templateLength))
		attrs := rec.Attributes()

		if p.cfg.DropRawBody {
			if isBytes {
				rec.Body().SetEmptyBytes().FromRaw([]byte(tmplStr))
			} else {
				rec.Body().SetStr(tmplStr)
			}
		} else {
			attrs.PutStr("razobill.template", tmplStr)
		}

		if p.cfg.IncludeHash {
			attrs.PutInt("razobill.formHash", int64(xxhash.Sum64String(tmplStr)))
		}

		// copy 64-bit integer parameters into razobill.ints slice attribute
		if cRecord.intCount > 0 && cRecord.intValues != nil {
			intsSlice := attrs.PutEmptySlice("razobill.ints")
			intsSlice.EnsureCapacity(int(cRecord.intCount))
			intPtrs := unsafe.Slice((*int64)(unsafe.Pointer(cRecord.intValues)), int(cRecord.intCount))
			for _, val := range intPtrs {
				intsSlice.AppendEmpty().SetInt(val)
			}
		}

		// copy string parameters into razobill.strs slice attribute
		if cRecord.strCount > 0 && cRecord.strValues != nil && cRecord.strLengths != nil {
			strsSlice := attrs.PutEmptySlice("razobill.strs")
			strsSlice.EnsureCapacity(int(cRecord.strCount))
			strPtrs := unsafe.Slice(cRecord.strValues, int(cRecord.strCount))
			strLens := unsafe.Slice(cRecord.strLengths, int(cRecord.strCount))
			for idx := 0; idx < int(cRecord.strCount); idx++ {
				sVal := C.GoStringN(strPtrs[idx], C.int(strLens[idx]))
				strsSlice.AppendEmpty().SetStr(sVal)
			}
		}
	} else {
		// record diagnostic error message if parsing fails
		rec.Attributes().PutStr("razobill.error", C.GoString(C.RzStatusMessage(status)))
	}
}

// ConsumeLogs batches log records, decomposes messages via C ABI, and forwards
// inv: reconstructed message matches original log line byte-for-byte
// in: ctx is cancellation context; ld contains telemetry resource logs
// out: forwards modified plog.Logs to downstream consumer
// own: frees temporary C memory via C.free and C.RzFree before returning
// thread: thread-safe across concurrent collector worker pipelines
// err: forwards downstream consumer errors or sets razobill.error attribute
func (p *RazobillProcessor) ConsumeLogs(ctx context.Context, ld plog.Logs) error {
	rls := ld.ResourceLogs()
	for i := 0; i < rls.Len(); i++ {
		if err := ctx.Err(); err != nil {
			return err
		}
		sls := rls.At(i).ScopeLogs()
		for j := 0; j < sls.Len(); j++ {
			records := sls.At(j).LogRecords()
			for k := 0; k < records.Len(); k++ {
				p.processRecord(records.At(k))
			}
		}
	}
	return p.nextConsumer.ConsumeLogs(ctx, ld)
}
