//go:build !windows

package main

import "time"

// Unter Linux und macOS gibt es nichts zu beenden und nicht zu warten.
const portRetry = time.Duration(0)

func killOldBridges() {}
