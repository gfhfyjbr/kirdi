import { useState, useCallback, useEffect, useRef } from "react"
import type { ConnectionStatus, VpnStats, VpnConfig } from "@/types/vpn"
import { defaultConfig, defaultStats } from "@/types/vpn"

// ── Tauri bridge detection ──────────────────────────────────────────────────
// In Tauri mode, we use invoke() and listen() from @tauri-apps/api.
// In standalone webview mode, we fall back to window.__kirdi (C++ bridge).
// In demo mode (neither available), we simulate connection for UI dev.

const isTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window

// Dynamic imports for Tauri API (only loaded when running in Tauri)
let tauriInvoke: ((cmd: string, args?: Record<string, unknown>) => Promise<unknown>) | null = null
let tauriListen: ((event: string, handler: (event: { payload: unknown }) => void) => Promise<() => void>) | null = null

if (isTauri) {
  import("@tauri-apps/api/core").then((mod) => {
    tauriInvoke = mod.invoke
  })
  import("@tauri-apps/api/event").then((mod) => {
    tauriListen = mod.listen
  })
}

// ── Legacy C++ webview bridge ───────────────────────────────────────────────
interface KirdiBridge {
  connect(config: string): void
  disconnect(): void
  getStatus(): string
}

declare global {
  interface Window {
    __kirdi?: KirdiBridge
    __kirdi_update?: (data: string) => void
  }
}

// ── Tauri event payload types ───────────────────────────────────────────────
interface VpnStatusUpdate {
  status?: ConnectionStatus
  error?: string
  stats?: Partial<VpnStats>
}

// ── Hook ────────────────────────────────────────────────────────────────────

export function useVpn() {
  const [status, setStatus] = useState<ConnectionStatus>("disconnected")
  const [stats, setStats] = useState<VpnStats>(defaultStats)
  const [config, setConfig] = useState<VpnConfig>(() => {
    const saved = localStorage.getItem("kirdi-config")
    if (saved) {
      try {
        return { ...defaultConfig, ...JSON.parse(saved) }
      } catch {
        return defaultConfig
      }
    }
    return defaultConfig
  })
  const [error, setError] = useState<string | null>(null)
  const uptimeInterval = useRef<ReturnType<typeof setInterval> | null>(null)
  const unlistenRef = useRef<(() => void) | null>(null)

  // Save config to localStorage on change
  useEffect(() => {
    localStorage.setItem("kirdi-config", JSON.stringify(config))
  }, [config])

  // ── Set up status update listener ─────────────────────────────────────
  useEffect(() => {
    if (isTauri) {
      // Tauri mode: listen for vpn-status events from Rust backend
      const setupListener = async () => {
        // Wait for dynamic imports to resolve
        const maxWait = 50
        let waited = 0
        while (!tauriListen && waited < maxWait) {
          await new Promise((r) => setTimeout(r, 100))
          waited++
        }
        if (!tauriListen) {
          console.error("Failed to load Tauri event API")
          return
        }

        unlistenRef.current?.()
        unlistenRef.current = await tauriListen("vpn-status", (event) => {
          const update = event.payload as VpnStatusUpdate
          if (update.status) setStatus(update.status)
          if (update.stats) setStats((prev) => ({ ...prev, ...update.stats }))
          if (update.error) setError(update.error)
        })
      }
      setupListener()
    } else {
      // Legacy C++ webview mode: register window.__kirdi_update callback
      window.__kirdi_update = (data: string) => {
        try {
          const update = JSON.parse(data) as VpnStatusUpdate
          if (update.status) setStatus(update.status)
          if (update.stats) setStats((prev) => ({ ...prev, ...update.stats }))
          if (update.error) setError(update.error)
        } catch (e) {
          console.error("Failed to parse kirdi update:", e)
        }
      }
    }

    return () => {
      unlistenRef.current?.()
      unlistenRef.current = null
      window.__kirdi_update = undefined
    }
  }, [])

  // Uptime counter when connected
  useEffect(() => {
    if (status === "connected") {
      uptimeInterval.current = setInterval(() => {
        setStats((prev) => ({ ...prev, uptime: prev.uptime + 1 }))
      }, 1000)
    } else {
      if (uptimeInterval.current) {
        clearInterval(uptimeInterval.current)
        uptimeInterval.current = null
      }
    }
    return () => {
      if (uptimeInterval.current) clearInterval(uptimeInterval.current)
    }
  }, [status])

  // ── Connect ───────────────────────────────────────────────────────────
  const connect = useCallback(async () => {
    setError(null)
    setStatus("connecting")
    setStats(defaultStats)

    const configJson = JSON.stringify(config)

    if (isTauri && tauriInvoke) {
      // Tauri mode: invoke Rust command
      try {
        await tauriInvoke("vpn_connect", { configJson })
      } catch (e) {
        setError(String(e))
        setStatus("error")
      }
    } else if (window.__kirdi) {
      // Legacy C++ webview mode
      window.__kirdi.connect(configJson)
    } else {
      // Demo mode: simulate connection for UI development
      setTimeout(() => {
        setStatus("connected")
        setStats({
          ping: 24,
          downloadSpeed: 12_500_000,
          uploadSpeed: 5_200_000,
          dataReceived: 0,
          dataSent: 0,
          uptime: 0,
          serverIp: config.serverHost,
          clientIp: "10.0.0.2",
        })
      }, 1500)
    }
  }, [config])

  // ── Disconnect ────────────────────────────────────────────────────────
  const disconnect = useCallback(async () => {
    setStatus("disconnecting")
    setError(null)

    if (isTauri && tauriInvoke) {
      // Tauri mode: invoke Rust command
      try {
        await tauriInvoke("vpn_disconnect")
      } catch (e) {
        setError(String(e))
      }
    } else if (window.__kirdi) {
      // Legacy C++ webview mode
      window.__kirdi.disconnect()
    } else {
      // Demo mode
      setTimeout(() => {
        setStatus("disconnected")
        setStats(defaultStats)
      }, 500)
    }
  }, [])

  const updateConfig = useCallback((partial: Partial<VpnConfig>) => {
    setConfig((prev) => ({ ...prev, ...partial }))
  }, [])

  return {
    status,
    stats,
    config,
    error,
    connect,
    disconnect,
    updateConfig,
  }
}
