import { motion, AnimatePresence } from "framer-motion"
import { Globe, MapPin, Wifi, WifiOff } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import type { ConnectionStatus, VpnStats } from "@/types/vpn"

interface ServerInfoProps {
  status: ConnectionStatus
  stats: VpnStats
  serverHost: string
  error: string | null
}

const statusBadge: Record<ConnectionStatus, { label: string; variant: "default" | "secondary" | "destructive" | "outline" }> = {
  disconnected: { label: "Disconnected", variant: "secondary" },
  connecting: { label: "Connecting", variant: "outline" },
  connected: { label: "Protected", variant: "default" },
  disconnecting: { label: "Disconnecting", variant: "outline" },
  error: { label: "Error", variant: "destructive" },
}

export function ServerInfo({ status, stats, serverHost, error }: ServerInfoProps) {
  const isConnected = status === "connected"
  const badgeConfig = statusBadge[status]

  return (
    <motion.div
      className="glass rounded-2xl p-5"
      initial={{ opacity: 0, y: 20 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.5, delay: 0.1 }}
      whileHover={{
        borderColor: "rgba(255, 255, 255, 0.1)",
        transition: { duration: 0.2 },
      }}
    >
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-3">
          <motion.div
            className="flex h-10 w-10 items-center justify-center rounded-xl"
            style={{
              backgroundColor: isConnected ? "rgba(34, 197, 94, 0.1)" : "rgba(113, 113, 122, 0.1)",
            }}
            animate={{
              backgroundColor: isConnected ? "rgba(34, 197, 94, 0.1)" : "rgba(113, 113, 122, 0.1)",
            }}
            transition={{ duration: 0.4 }}
          >
            <AnimatePresence mode="wait">
              <motion.div
                key={isConnected ? "on" : "off"}
                initial={{ opacity: 0, rotate: -20 }}
                animate={{ opacity: 1, rotate: 0 }}
                exit={{ opacity: 0, rotate: 20 }}
                transition={{ duration: 0.2 }}
              >
                {isConnected ? (
                  <Wifi size={20} className="text-green-400" />
                ) : (
                  <WifiOff size={20} className="text-zinc-500" />
                )}
              </motion.div>
            </AnimatePresence>
          </motion.div>

          <div className="flex flex-col gap-0.5">
            <span className="text-sm font-semibold text-foreground">kirdi VPN</span>
            <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
              <Globe size={10} />
              <span>{serverHost || "Not configured"}</span>
            </div>
          </div>
        </div>

        <motion.div
          whileHover={{ scale: 1.05 }}
          whileTap={{ scale: 0.95 }}
        >
          <Badge variant={badgeConfig.variant} className="text-[10px] uppercase tracking-wider">
            {badgeConfig.label}
          </Badge>
        </motion.div>
      </div>

      {/* Connected info */}
      <AnimatePresence>
        {isConnected && (
          <motion.div
            className="mt-4 flex items-center gap-4 border-t border-white/[0.04] pt-4"
            initial={{ opacity: 0, height: 0 }}
            animate={{ opacity: 1, height: "auto" }}
            exit={{ opacity: 0, height: 0 }}
            transition={{ duration: 0.3 }}
          >
            <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
              <MapPin size={10} className="text-cyan-400" />
              <span>Server: <span className="text-foreground font-mono">{stats.serverIp || "—"}</span></span>
            </div>
            <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
              <MapPin size={10} className="text-green-400" />
              <span>Client: <span className="text-foreground font-mono">{stats.clientIp || "—"}</span></span>
            </div>
          </motion.div>
        )}
      </AnimatePresence>

      {/* Error message */}
      <AnimatePresence>
        {error && (
          <motion.div
            className="mt-3 rounded-lg bg-red-500/10 px-3 py-2 text-xs text-red-400"
            initial={{ opacity: 0, height: 0 }}
            animate={{ opacity: 1, height: "auto" }}
            exit={{ opacity: 0, height: 0 }}
            transition={{ duration: 0.3 }}
          >
            {error}
          </motion.div>
        )}
      </AnimatePresence>
    </motion.div>
  )
}
