import { motion } from "framer-motion"
import { ConnectButton } from "@/components/ConnectButton"
import { StatusBar } from "@/components/StatusBar"
import { ServerInfo } from "@/components/ServerInfo"
import { SettingsPanel } from "@/components/SettingsPanel"
import { useVpn } from "@/hooks/useVpn"

function App() {
  const { status, stats, config, error, connect, disconnect, updateConfig } = useVpn()

  return (
    <div className="flex h-screen w-screen flex-col items-center bg-background overflow-hidden">
      {/* Ambient background gradient */}
      <div className="pointer-events-none fixed inset-0 z-0">
        <motion.div
          className="absolute left-1/2 top-1/3 h-[500px] w-[500px] -translate-x-1/2 -translate-y-1/2 rounded-full opacity-[0.03]"
          animate={{
            background:
              status === "connected"
                ? "radial-gradient(circle, #22c55e 0%, transparent 70%)"
                : status === "connecting"
                  ? "radial-gradient(circle, #22d3ee 0%, transparent 70%)"
                  : status === "error"
                    ? "radial-gradient(circle, #ef4444 0%, transparent 70%)"
                    : "radial-gradient(circle, #71717a 0%, transparent 70%)",
          }}
          transition={{ duration: 1.5, ease: "easeInOut" }}
        />
      </div>

      {/* Main content */}
      <motion.div
        className="relative z-10 flex w-full max-w-[520px] flex-col items-center gap-6 px-6 py-8"
        initial={{ opacity: 0 }}
        animate={{ opacity: 1 }}
        transition={{ duration: 0.6 }}
      >
        {/* Logo / Title */}
        <motion.div
          className="flex flex-col items-center gap-1"
          initial={{ opacity: 0, y: -10 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.5 }}
        >
          <h1 className="text-lg font-bold tracking-tight text-foreground">
            kirdi
          </h1>
          <p className="text-[10px] uppercase tracking-[0.3em] text-muted-foreground">
            L3 VPN Tunnel
          </p>
        </motion.div>

        {/* Server info */}
        <ServerInfo
          status={status}
          stats={stats}
          serverHost={config.serverHost}
          error={error}
        />

        {/* Connect button */}
        <div className="py-4">
          <ConnectButton
            status={status}
            onConnect={connect}
            onDisconnect={disconnect}
          />
        </div>

        {/* Stats */}
        <StatusBar status={status} stats={stats} />

        {/* Settings */}
        <SettingsPanel
          config={config}
          status={status}
          onConfigChange={updateConfig}
        />

        {/* Version */}
        <motion.p
          className="mt-auto text-[9px] text-muted-foreground/40"
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          transition={{ delay: 0.8 }}
        >
          kirdi v0.1.0
        </motion.p>
      </motion.div>
    </div>
  )
}

export default App
