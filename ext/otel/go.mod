// =============================================================================
// OpenTelemetry Collector processor module for razobill
//
// defines Go module dependencies for embedding razobill processor into custom
// OpenTelemetry Collector distributions
// =============================================================================

module github.com/razobill/razobill/ext/otel

go 1.26.0

require (
	github.com/cespare/xxhash/v2 v2.3.0
	go.opentelemetry.io/collector/component v0.111.0
	go.opentelemetry.io/collector/consumer v1.69.0
	go.opentelemetry.io/collector/consumer/consumertest v0.163.0
	go.opentelemetry.io/collector/pdata v1.69.0
	go.opentelemetry.io/collector/processor v0.111.0
)

require (
	github.com/hashicorp/go-version v1.9.0 // indirect
	github.com/json-iterator/go v1.1.12 // indirect
	github.com/modern-go/concurrent v0.0.0-20180306012644-bacd9c7ef1dd // indirect
	github.com/modern-go/reflect2 v1.0.3-0.20250322232337-35a7c28c31ee // indirect
	go.opentelemetry.io/collector/config/configtelemetry v0.111.0 // indirect
	go.opentelemetry.io/collector/consumer/xconsumer v0.163.0 // indirect
	go.opentelemetry.io/collector/featuregate v1.69.0 // indirect
	go.opentelemetry.io/collector/internal/globalsignal v0.111.0 // indirect
	go.opentelemetry.io/collector/pdata/pprofile v0.163.0 // indirect
	go.opentelemetry.io/collector/pipeline v0.111.0 // indirect
	go.opentelemetry.io/otel v1.47.0 // indirect
	go.opentelemetry.io/otel/metric v1.47.0 // indirect
	go.opentelemetry.io/otel/trace v1.47.0 // indirect
	go.uber.org/multierr v1.11.0 // indirect
	go.uber.org/zap v1.27.0 // indirect
)
