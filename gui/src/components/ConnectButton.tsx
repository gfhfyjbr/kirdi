import { motion, AnimatePresence } from "framer-motion"
import { Power, Loader2, ShieldCheck, ShieldOff } from "lucide-react"
import type { ConnectionStatus } from "@/types/vpn"

interface ConnectButtonProps {
  status: ConnectionStatus
  onConnect: () => void
  onDisconnect: () => void
}

const statusConfig = {
  disconnected: {
    icon: Power,
    label: "Connect",
    ringColor: "rgba(113, 113, 122, 0.3)",
    glowColor: "rgba(113, 113, 122, 0.1)",
    bgColor: "rgba(39, 39, 42, 0.5)",
    iconColor: "#a1a1aa",
  },
  connecting: {
    icon: Loader2,
    label: "Connecting...",
    ringColor: "rgba(34, 211, 238, 0.5)",
    glowColor: "rgba(34, 211, 238, 0.15)",
    bgColor: "rgba(34, 211, 238, 0.1)",
    iconColor: "#22d3ee",
  },
  connected: {
    icon: ShieldCheck,
    label: "Connected",
    ringColor: "rgba(34, 197, 94, 0.6)",
    glowColor: "rgba(34, 197, 94, 0.2)",
    bgColor: "rgba(34, 197, 94, 0.1)",
    iconColor: "#4ade80",
  },
  disconnecting: {
    icon: Loader2,
    label: "Disconnecting...",
    ringColor: "rgba(239, 68, 68, 0.5)",
    glowColor: "rgba(239, 68, 68, 0.15)",
    bgColor: "rgba(239, 68, 68, 0.1)",
    iconColor: "#ef4444",
  },
  error: {
    icon: ShieldOff,
    label: "Error",
    ringColor: "rgba(239, 68, 68, 0.6)",
    glowColor: "rgba(239, 68, 68, 0.2)",
    bgColor: "rgba(239, 68, 68, 0.1)",
    iconColor: "#ef4444",
  },
}

export function ConnectButton({ status, onConnect, onDisconnect }: ConnectButtonProps) {
  const config = statusConfig[status]
  const isLoading = status === "connecting" || status === "disconnecting"
  const isConnected = status === "connected"
  const isClickable = status === "disconnected" || status === "connected" || status === "error"

  const handleClick = () => {
    if (!isClickable) return
    if (isConnected) {
      onDisconnect()
    } else {
      onConnect()
    }
  }

  return (
    <div className="flex flex-col items-center gap-6">
      {/* Outer glow ring */}
      <motion.div
        className="relative"
        animate={{
          filter: `drop-shadow(0 0 30px ${config.glowColor}) drop-shadow(0 0 60px ${config.glowColor})`,
        }}
        transition={{ duration: 0.6, ease: "easeInOut" }}
      >
        {/* Pulsing ring for connected state */}
        <AnimatePresence>
          {isConnected && (
            <motion.div
              className="absolute inset-[-8px] rounded-full"
              initial={{ opacity: 0, scale: 0.8 }}
              animate={{
                opacity: [0.4, 0.1, 0.4],
                scale: [1, 1.15, 1],
              }}
              exit={{ opacity: 0, scale: 0.8 }}
              transition={{
                duration: 2.5,
                repeat: Infinity,
                ease: "easeInOut",
              }}
              style={{
                border: `2px solid ${config.ringColor}`,
              }}
            />
          )}
        </AnimatePresence>

        {/* Spinning ring for loading states */}
        <AnimatePresence>
          {isLoading && (
            <motion.div
              className="absolute inset-[-6px] rounded-full"
              initial={{ opacity: 0, rotate: 0 }}
              animate={{ opacity: 1, rotate: 360 }}
              exit={{ opacity: 0 }}
              transition={{
                rotate: { duration: 2, repeat: Infinity, ease: "linear" },
                opacity: { duration: 0.3 },
              }}
              style={{
                border: `2px solid transparent`,
                borderTopColor: config.ringColor,
                borderRightColor: config.ringColor,
              }}
            />
          )}
        </AnimatePresence>

        {/* Main button */}
        <motion.button
          onClick={handleClick}
          disabled={!isClickable}
          className="relative flex h-32 w-32 items-center justify-center rounded-full border border-white/[0.06] outline-none transition-colors disabled:cursor-not-allowed"
          style={{ backgroundColor: config.bgColor }}
          whileHover={isClickable ? { scale: 1.05 } : {}}
          whileTap={isClickable ? { scale: 0.95 } : {}}
          animate={{
            boxShadow: `0 0 40px ${config.glowColor}, inset 0 0 40px ${config.glowColor}`,
            borderColor: config.ringColor,
          }}
          transition={{ duration: 0.4, ease: "easeOut" }}
        >
          <AnimatePresence mode="wait">
            <motion.div
              key={status}
              initial={{ opacity: 0, scale: 0.5 }}
              animate={{ opacity: 1, scale: 1 }}
              exit={{ opacity: 0, scale: 0.5 }}
              transition={{ duration: 0.25, ease: "backOut" }}
              className={isLoading ? "animate-spin" : ""}
              style={isLoading ? { animationDuration: "1.5s" } : {}}
            >
              <config.icon
                size={44}
                strokeWidth={2}
                style={{ color: config.iconColor }}
              />
            </motion.div>
          </AnimatePresence>
        </motion.button>
      </motion.div>

      {/* Status label */}
      <AnimatePresence mode="wait">
        <motion.span
          key={status}
          className="text-sm font-medium tracking-wider uppercase"
          style={{ color: config.iconColor }}
          initial={{ opacity: 0, y: 8 }}
          animate={{ opacity: 1, y: 0 }}
          exit={{ opacity: 0, y: -8 }}
          transition={{ duration: 0.2 }}
        >
          {config.label}
        </motion.span>
      </AnimatePresence>
    </div>
  )
}
