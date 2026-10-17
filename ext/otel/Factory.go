// =============================================================================
// OpenTelemetry Collector processor factory for razobill
//
// registers the razobill log processor component with the collector registry
// at Beta stability level
// =============================================================================

package razobillprocessor

import (
	"context"

	"go.opentelemetry.io/collector/component"
	"go.opentelemetry.io/collector/consumer"
	"go.opentelemetry.io/collector/processor"
)

// typeStr defines the component identifier ("razobill") in collector pipelines
var typeStr = component.MustNewType("razobill")

// NewFactory constructs the OpenTelemetry processor factory for razobill
// out: processor.Factory configured with default config and log constructor
func NewFactory() processor.Factory {
	return processor.NewFactory(
		typeStr,
		createDefaultConfigFunc,
		processor.WithLogs(createLogsProcessor, component.StabilityLevelBeta),
	)
}

// createDefaultConfigFunc returns component configuration with default settings
func createDefaultConfigFunc() component.Config {
	return createDefaultConfig()
}

// createLogsProcessor constructs a new log record processor instance
// in: cfg must be a valid *Config pointer; next is downstream log consumer
// out: processor.Logs implementation wrapping downstream consumer
// err: returns non-nil error if processor initialization fails
func createLogsProcessor(
	_ context.Context,
	_ processor.Settings,
	cfg component.Config,
	next consumer.Logs,
) (processor.Logs, error) {
	oCfg := cfg.(*Config)
	return newRazobillProcessor(oCfg, next)
}
