# Lobby server

Small Node.js server (no dependencies, Node 18+) that lists public rooms and helps with NAT hole-punching.
Same API as the former Cloudflare worker: `/rooms`, `/announce`, `/punch`, `/punches`, `/close`, plus `/health`.

State is kept in memory only (rooms expire after 90 s unless the host re-announces), so a restart is harmless.

## Run

```
PORT=8080 node server.js
```

Environment variables: `PORT`, `HOST`, `TRUST_PROXY=1` (only behind a reverse proxy such as Caddy/nginx), `STRICT_OWNER=0`, `STRICT_PUNCH_IP=0`.

## Run as a service (systemd)

`/etc/systemd/system/melonds-lobby.service`:

```
[Unit]
Description=melonDS lobby server
After=network.target

[Service]
WorkingDirectory=/opt/melonds-lobby
ExecStart=/usr/bin/node server.js
Environment=PORT=8080
Restart=always
User=nobody

[Install]
WantedBy=multi-user.target
```

```
sudo systemctl daemon-reload
sudo systemctl enable --now melonds-lobby
curl http://localhost:8080/health
```

## Open the port (Oracle Cloud)

1. In the Oracle console, add an **ingress rule** for TCP port 8080 (source `0.0.0.0/0`) to the subnet's security list.
2. On the VM itself, allow the port in the OS firewall:
   - Ubuntu: `sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 8080 -j ACCEPT && sudo netfilter-persistent save`
   - Oracle Linux: `sudo firewall-cmd --permanent --add-port=8080/tcp && sudo firewall-cmd --reload`
3. Test from another machine: `curl http://<public-ip>:8080/health`
