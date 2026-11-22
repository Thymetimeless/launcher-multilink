// Speed test built on Plexo's own network layer (plexo/src/main/network, unmodified):
// listActiveInterfaces finds and classifies the networks, connectThrough measures each one's
// TCP+TLS handshake, and StreamConnection carries every download stream through its network
// exactly as a Plexo download does. Each network is tested alone, then all of them together.
//
// node dist/speedtest.mjs [--streams 8] [--seconds 10] [--url <file>] [--networks "Ethernet,Wi-Fi"] [--all]
import { listActiveInterfaces } from './plexo/src/main/network/interfaces'
import { connectThrough, StreamConnection } from './plexo/src/main/network/routes'
import type { NetworkInterfaceInfo } from './plexo/src/shared/types'

const BLOCK_BYTES = 32 * 1024 * 1024
const TIMEOUT_MS = 15_000
const HOYO_API =
  'https://sg-hyp-api.hoyoverse.com/hyp/hyp-connect/api/getGamePackages?launcher_id=VYTpXlbWo8'

interface Options {
  streams: number
  seconds: number
  url?: string
  networks?: string[]
  all: boolean
}

function parseArgs(argv: string[]): Options {
  const opts: Options = { streams: 8, seconds: 10, all: false }
  for (let i = 0; i < argv.length; i++) {
    const value = (): string => {
      const v = argv[++i]
      if (v === undefined) throw new Error(`${argv[i - 1]} needs a value`)
      return v
    }
    const flag = argv[i]
    if (flag === '--streams') opts.streams = Number(value())
    else if (flag === '--seconds') opts.seconds = Number(value())
    else if (flag === '--url') opts.url = value()
    else if (flag === '--networks') opts.networks = value().split(',').map((s) => s.trim()).filter(Boolean)
    else if (flag === '--all') opts.all = true
    else throw new Error(`Unknown option ${flag}`)
  }
  if (!Number.isInteger(opts.streams) || opts.streams < 1 || opts.streams > 32) throw new Error('--streams must be 1..32')
  if (!Number.isFinite(opts.seconds) || opts.seconds < 3 || opts.seconds > 120) throw new Error('--seconds must be 3..120')
  return opts
}

async function defaultUrl(): Promise<string> {
  const res = await fetch(HOYO_API, { signal: AbortSignal.timeout(TIMEOUT_MS) })
  const body = (await res.json()) as {
    data?: { game_packages?: { game: { biz: string }; main: { major: { game_pkgs: { url: string }[] } } }[] }
  }
  const url = body.data?.game_packages?.find((g) => g.game.biz === 'hkrpg_global')?.main.major.game_pkgs[0]?.url
  if (!url) throw new Error('No test file from the HoYoPlay API; pass --url')
  return url
}

const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms))
const median = (xs: number[]): number => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)]
const ipv4 = (n: NetworkInterfaceInfo) => n.addresses.find((a) => a.family === 4)

/** Median TCP+TLS handshake through Plexo's connectThrough, or null if every try failed. */
async function handshakeMs(net: NetworkInterfaceInfo, host: string, port: number): Promise<number | null> {
  const times: number[] = []
  for (let i = 0; i < 3; i++) {
    const started = performance.now()
    try {
      const socket = await connectThrough(net, host, port, true, 5000, AbortSignal.timeout(6000))
      times.push(performance.now() - started)
      socket.destroy()
    } catch {
      /* counted as a failed try */
    }
  }
  return times.length ? Math.round(median(times)) : null
}

interface Lane {
  net: NetworkInterfaceInfo
  bytes: number
  error?: string
}

/** Runs `streams` StreamConnections per lane for `seconds`; returns bytes per lane inside the window. */
async function measure(lanes: Lane[], url: string, streams: number, seconds: number): Promise<void> {
  const stop = new AbortController()
  let fileSize = 1024 * 1024 * 1024 // until the first Content-Range says otherwise
  let nextBlock = 0
  const block = (): [number, number] => {
    const blocks = Math.max(1, Math.floor(fileSize / BLOCK_BYTES))
    const start = (nextBlock++ % blocks) * BLOCK_BYTES
    return [start, Math.min(start + BLOCK_BYTES, fileSize) - 1]
  }

  const stream = async (lane: Lane, conn: StreamConnection): Promise<void> => {
    let target = new URL(url)
    let failures = 0
    while (!stop.signal.aborted) {
      const [start, end] = block()
      try {
        const { req, res } = await conn.request(target, { Range: `bytes=${start}-${end}` }, stop.signal)
        const status = res.statusCode ?? 0
        if (status >= 300 && status < 400 && res.headers.location) {
          res.resume()
          target = new URL(res.headers.location, target)
          continue
        }
        if (status !== 206) {
          res.resume()
          throw new Error(`server answered ${status} to a range request`)
        }
        const total = Number(/\/(\d+)$/.exec(res.headers['content-range'] ?? '')?.[1])
        if (Number.isFinite(total) && total > 0) fileSize = total
        await new Promise<void>((resolve) => {
          const onStop = (): void => void req.destroy()
          stop.signal.addEventListener('abort', onStop, { once: true })
          res.on('data', (chunk: Buffer) => {
            if (!stop.signal.aborted) lane.bytes += chunk.length
          })
          res.once('close', () => {
            stop.signal.removeEventListener('abort', onStop)
            resolve()
          })
          res.once('error', () => undefined) // 'close' follows and resolves
        })
        failures = 0
      } catch (error) {
        if (stop.signal.aborted) return
        lane.error = error instanceof Error ? error.message : String(error)
        if (++failures >= 5) return
        await sleep(300)
      }
    }
  }

  const conns: StreamConnection[] = []
  const running: Promise<void>[] = []
  for (let i = 0; i < streams; i++) {
    for (const lane of lanes) {
      const conn = new StreamConnection(() => lane.net, { timeoutMs: TIMEOUT_MS, connectTimeoutMs: 5000 })
      conns.push(conn)
      running.push(stream(lane, conn))
    }
  }
  await sleep(seconds * 1000)
  stop.abort()
  await Promise.allSettled(running)
  for (const conn of conns) conn.close()
}

const mbit = (bytes: number, seconds: number): number => Math.round((bytes * 8) / seconds / 1e5) / 10

async function main(): Promise<void> {
  const opts = parseArgs(process.argv.slice(2))
  const url = opts.url ?? (await defaultUrl())
  const target = new URL(url)
  const port = Number(target.port) || (target.protocol === 'https:' ? 443 : 80)

  let nets = (await listActiveInterfaces()).filter((n) => ipv4(n))
  if (opts.networks) nets = nets.filter((n) => opts.networks!.some((w) => w.toLowerCase() === n.displayName.toLowerCase()))
  else if (!opts.all) nets = nets.filter((n) => n.kind !== 'other' && n.kind !== 'bridge')
  if (nets.length === 0) throw new Error('No network to test (try --all or --networks)')

  console.log(`Server : ${target.host}`)
  console.log(`Test   : ${opts.streams} Plexo streams per network, ${opts.seconds} s per run\n`)

  const rows: { network: string; kind: string; address: string; handshake: string; mbit: number; note: string }[] = []
  let best = 0
  const reachable: NetworkInterfaceInfo[] = []
  for (const net of nets) {
    process.stdout.write(`Testing ${net.displayName} ... `)
    const hs = await handshakeMs(net, target.hostname, port)
    if (hs === null) {
      // Plexo lists VPN/virtual adapters too (e.g. Radmin VPN as 'ethernet'); one that can't reach the server is left out.
      console.log('cannot reach the server, skipped')
      rows.push({ network: net.displayName, kind: net.kind, address: ipv4(net)!.address,
        handshake: 'fail', mbit: 0, note: 'no route to the server - skipped' })
      continue
    }
    reachable.push(net)
    const lane: Lane = { net, bytes: 0 }
    await measure([lane], url, opts.streams, opts.seconds)
    const speed = mbit(lane.bytes, opts.seconds)
    best = Math.max(best, speed)
    console.log(`${speed} Mbit/s`)
    rows.push({ network: net.displayName, kind: net.kind, address: ipv4(net)!.address,
      handshake: `${hs} ms`, mbit: speed, note: lane.bytes ? '' : (lane.error ?? '') })
  }
  nets = reachable

  let combined = 0
  if (nets.length >= 2) {
    process.stdout.write('Testing all networks combined ... ')
    const lanes: Lane[] = nets.map((net) => ({ net, bytes: 0 }))
    await measure(lanes, url, opts.streams, opts.seconds)
    combined = mbit(lanes.reduce((s, l) => s + l.bytes, 0), opts.seconds)
    console.log(`${combined} Mbit/s`)
    const share = lanes.map((l) => `${l.net.displayName} ${mbit(l.bytes, opts.seconds)}`).join(' + ')
    rows.push({ network: 'COMBINED', kind: '', address: '', handshake: '', mbit: combined, note: share })
  }
  console.log()
  console.table(rows.map((r) => ({ Network: r.network, Kind: r.kind, Address: r.address,
    Handshake: r.handshake, 'Mbit/s': r.mbit, Note: r.note })))

  if (nets.length < 2) return console.log('Only one network: nothing to combine.')
  const gain = best > 0 ? Math.round((combined / best - 1) * 100) : 0
  console.log(`Combined vs best single network: ${gain} %`)
  // Plexo's own same-subnet signal (NetworkAddress.subnet): same subnet = same router, usually one line.
  const bySubnet = new Map<string, string[]>()
  for (const n of nets) {
    const subnet = ipv4(n)?.subnet
    if (subnet) bySubnet.set(subnet, [...(bySubnet.get(subnet) ?? []), n.displayName])
  }
  for (const [subnet, names] of bySubnet)
    if (names.length > 1) console.log(`Note: ${names.join(' + ')} are on the same subnet ${subnet} - likely one internet line.`)
  // shortcut: fixed 15 % threshold from the run-to-run CDN variance measured on this machine.
  console.log(gain >= 15 ? 'Verdict: combining adds real speed.' : 'Verdict: no real gain - the networks share one internet line (or the server is the limit).')
}

main().then(
  () => process.exit(0),
  (error) => {
    console.error(error instanceof Error ? error.message : error)
    process.exit(1)
  }
)
