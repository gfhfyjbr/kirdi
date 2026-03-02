import { motion } from "framer-motion"
import { Settings, Server, Key, Route, Globe } from "lucide-react"
import { Input } from "@/components/ui/input"
import { Switch } from "@/components/ui/switch"
import { Separator } from "@/components/ui/separator"
import type { ConnectionStatus, VpnConfig } from "@/types/vpn"

interface SettingsPanelProps {
  config: VpnConfig
  status: ConnectionStatus
  onConfigChange: (partial: Partial<VpnConfig>) => void
}

interface SettingRowProps {
  icon: React.ReactNode
  label: string
  children: React.ReactNode
  index: number
}

function SettingRow({ icon, label, children, index }: SettingRowProps) {
  return (
    <motion.div
      className="flex items-center justify-between gap-4 py-1"
      initial={{ opacity: 0, x: -10 }}
      animate={{ opacity: 1, x: 0 }}
      transition={{ delay: index * 0.05, duration: 0.3 }}
    >
      <div className="flex items-center gap-2 text-xs text-muted-foreground">
        {icon}
        <span>{label}</span>
      </div>
      <div className="flex-1 max-w-[220px]">{children}</div>
    </motion.div>
  )
}

export function SettingsPanel({ config, status, onConfigChange }: SettingsPanelProps) {
  const isEditable = status === "disconnected" || status === "error"

  return (
    <motion.div
      className="glass w-full rounded-2xl"
      initial={{ opacity: 0, y: 20 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.5, delay: 0.2 }}
    >
      {/* Header */}
      <div className="flex items-center gap-2.5 px-4 pt-3 pb-0">
        <Settings size={14} className="text-muted-foreground" />
        <span className="text-xs font-semibold text-foreground">Settings</span>
      </div>

      {/* Content */}
      <div className="px-4 pb-3 pt-2">
        <Separator className="mb-2 bg-white/[0.04]" />

        <div className="flex flex-col gap-0">
          <SettingRow icon={<Server size={14} />} label="Server" index={0}>
            <Input
              value={config.serverHost}
              onChange={e => onConfigChange({ serverHost: e.target.value })}
              disabled={!isEditable}
              placeholder="vpn.example.com"
              className="h-7 bg-black/30 border-white/[0.06] text-xs font-mono focus-visible:ring-cyan-500/30"
            />
          </SettingRow>

          <SettingRow icon={<Route size={14} />} label="WS Path" index={1}>
            <Input
              value={config.wsPath}
              onChange={e => onConfigChange({ wsPath: e.target.value })}
              disabled={!isEditable}
              placeholder="/ws/"
              className="h-7 bg-black/30 border-white/[0.06] text-xs font-mono focus-visible:ring-cyan-500/30"
            />
          </SettingRow>

          <SettingRow icon={<Key size={14} />} label="Auth Token" index={2}>
            <Input
              type="password"
              value={config.authToken}
              onChange={e => onConfigChange({ authToken: e.target.value })}
              disabled={!isEditable}
              placeholder="Enter token..."
              className="h-7 bg-black/30 border-white/[0.06] text-xs font-mono focus-visible:ring-cyan-500/30"
            />
          </SettingRow>

          <SettingRow icon={<Globe size={14} />} label="DNS" index={3}>
            <Input
              value={config.dns}
              onChange={e => onConfigChange({ dns: e.target.value })}
              disabled={!isEditable}
              placeholder="1.1.1.1"
              className="h-7 bg-black/30 border-white/[0.06] text-xs font-mono focus-visible:ring-cyan-500/30"
            />
          </SettingRow>

          <Separator className="my-1 bg-white/[0.04]" />

          <SettingRow icon={<span className="text-xs">TLS</span>} label="Use TLS" index={4}>
            <div className="flex justify-end">
              <Switch
                checked={config.useTls}
                onCheckedChange={v => onConfigChange({ useTls: v })}
                disabled={!isEditable}
              />
            </div>
          </SettingRow>

          <SettingRow icon={<Route size={14} />} label="Auto Route" index={5}>
            <div className="flex justify-end">
              <Switch
                checked={config.autoRoute}
                onCheckedChange={v => onConfigChange({ autoRoute: v })}
                disabled={!isEditable}
              />
            </div>
          </SettingRow>
        </div>

        {!isEditable && (
          <motion.p
            className="mt-3 text-[10px] text-muted-foreground/60 text-center"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
          >
            Disconnect to edit settings
          </motion.p>
        )}
      </div>
    </motion.div>
  )
}
