export type ConnectionStatus = "disconnected" | "connecting" | "connected" | "disconnecting" | "error"

export interface VpnStats {
  ping: number          // ms
  downloadSpeed: number // bytes/sec
  uploadSpeed: number   // bytes/sec
  dataReceived: number  // total bytes
  dataSent: number      // total bytes
  uptime: number        // seconds
  serverIp: string
  clientIp: string
}

export interface VpnConfig {
  serverHost: string
  serverPort: number
  wsPath: string
  authToken: string
  useTls: boolean
  autoRoute: boolean
  dns: string
}

export const defaultConfig: VpnConfig = {
  serverHost: "vpn.goidamax.com",
  serverPort: 443,
  wsPath: "/1a7d73dd0d474e99/",
  authToken: "",
  useTls: true,
  autoRoute: true,
  dns: "1.1.1.1",
}

export const defaultStats: VpnStats = {
  ping: 0,
  downloadSpeed: 0,
  uploadSpeed: 0,
  dataReceived: 0,
  dataSent: 0,
  uptime: 0,
  serverIp: "",
  clientIp: "",
}
