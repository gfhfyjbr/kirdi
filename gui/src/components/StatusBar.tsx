import { motion } from "framer-motion"
import { Activity, ArrowDown, ArrowUp, Clock, Zap } from "lucide-react"
import type { ConnectionStatus, VpnStats } from "@/types/vpn"

interface StatusBarProps {
  status: ConnectionStatus
  stats: VpnStats
}

function formatBytes(bytes: number): string {
  if (bytes === 0) return "0 B"
  const k = 1024
  const sizes = ["B", "KB", "MB", "GB", "TB"]
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return `${(bytes / Math.pow(k, i)).toFixed(1)} ${sizes[i]}`
}

function formatSpeed(bytesPerSec: number): string {
  if (bytesPerSec === 0) return "0 B/s"
  const k = 1024
  const sizes = ["B/s", "KB/s", "MB/s", "GB/s"]
  const i = Math.floor(Math.log(bytesPerSec) / Math.log(k))
  return `${(bytesPerSec / Math.pow(k, i)).toFixed(1)} ${sizes[i]}`
}

function formatUptime(seconds: number): string {
  if (seconds === 0) return "00:00:00"
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = seconds % 60
  return [h, m, s].map(v => v.toString().padStart(2, "0")).join(":")
}

interface StatItemProps {
  icon: React.ReactNode
  label: string
  value: string
  color: string
  index: number
}

function StatItem({ icon, label, value, color, index }: StatItemProps) {
  return (
    <motion.div
      className="glass flex flex-col items-center gap-1.5 rounded-xl px-4 py-3"
      initial={{ opacity: 0, y: 20 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ delay: index * 0.08, duration: 0.4, ease: "backOut" }}
      whileHover={{
        y: -4,
        transition: { duration: 0.25, ease: "easeOut" },
      }}
    >
      <div className="flex items-center gap-1.5 text-muted-foreground">
        <span style={{ color }}>{icon}</span>
        <span className="text-[10px] uppercase tracking-widest">{label}</span>
      </div>
      <motion.span
        className="text-base font-semibold tabular-nums"
        style={{ color }}
        key={value}
        initial={{ opacity: 0.6 }}
        animate={{ opacity: 1 }}
        transition={{ duration: 0.15 }}
      >
        {value}
      </motion.span>
    </motion.div>
  )
}

export function StatusBar({ status, stats }: StatusBarProps) {
  const isConnected = status === "connected"

  const items = [
    {
      icon: <Zap size={12} />,
      label: "Ping",
      value: isConnected ? `${stats.ping} ms` : "—",
      color: "#22d3ee",
    },
    {
      icon: <ArrowDown size={12} />,
      label: "Download",
      value: isConnected ? formatSpeed(stats.downloadSpeed) : "—",
      color: "#22c55e",
    },
    {
      icon: <ArrowUp size={12} />,
      label: "Upload",
      value: isConnected ? formatSpeed(stats.uploadSpeed) : "—",
      color: "#a78bfa",
    },
    {
      icon: <Activity size={12} />,
      label: "Data",
      value: isConnected
        ? `${formatBytes(stats.dataReceived)} / ${formatBytes(stats.dataSent)}`
        : "—",
      color: "#f59e0b",
    },
    {
      icon: <Clock size={12} />,
      label: "Uptime",
      value: isConnected ? formatUptime(stats.uptime) : "—",
      color: "#64748b",
    },
  ]

  return (
    <motion.div
      className="grid grid-cols-5 gap-2"
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      transition={{ duration: 0.4 }}
    >
      {items.map((item, i) => (
        <StatItem key={item.label} {...item} index={i} />
      ))}
    </motion.div>
  )
}
