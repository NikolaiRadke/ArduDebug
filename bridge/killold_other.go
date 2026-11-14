//go:build !windows

package main

import "time"

// On Linux and macOS there is nothing to kill and nothing to wait for.
const portRetry = time.Duration(0)

func killOldBridges() {}
