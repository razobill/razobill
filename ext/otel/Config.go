// =============================================================================
// OpenTelemetry Collector processor configuration for razobill
//
// defines configuration schema and defaults for embedding razobill processor
// into OpenTelemetry Collector pipelines
// =============================================================================

package razobillprocessor

import "errors"

// Config defines configuration options for the razobill log processor
// in: Mode must be "semantic"
// out: validated processor configuration struct
type Config struct {
	Mode        string `mapstructure:"mode"`
	DropRawBody bool   `mapstructure:"drop_raw_body"`
	IncludeHash bool   `mapstructure:"include_hash"`
}

// CreateDefaultConfig returns processor settings with production defaults
// out: configuration initialized to semantic mode with raw body dropped
// own: returns newly allocated Config pointer
func CreateDefaultConfig() *Config {
	return &Config{
		Mode:        "semantic",
		DropRawBody: true,
		IncludeHash: true,
	}
}

// createDefaultConfig wraps CreateDefaultConfig for factory compatibility
func createDefaultConfig() *Config {
	return CreateDefaultConfig()
}

// Validate checks configuration fields and ensures valid scan modes
// inv: cfg.Mode is "semantic"
// err: returns error if cfg.Mode is not "semantic"
func (cfg *Config) Validate() error {
	if cfg.Mode != "semantic" {
		return errors.New("mode must be 'semantic'")
	}
	return nil
}
