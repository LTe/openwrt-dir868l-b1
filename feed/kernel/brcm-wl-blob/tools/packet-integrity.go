// End-to-end payload and sequence validation; not a throughput benchmark.
package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"syscall"
	"time"
)

func fail(e error) {
	if e != nil {
		fmt.Fprintln(os.Stderr, e)
		os.Exit(1)
	}
}
func num(s string) int { v, e := strconv.Atoi(s); fail(e); return v }
func fill(b []byte, seq int) {
	binary.BigEndian.PutUint64(b, uint64(seq))
	for i := 8; i < len(b); i++ {
		b[i] = byte(uint64(seq)*131 + uint64(i)*17)
	}
}
func main() {
	if len(os.Args) != 7 {
		fmt.Fprintln(os.Stderr, "send|recv tcp|udp address size count mbps")
		os.Exit(2)
	}
	mode, proto, addr := os.Args[1], os.Args[2], os.Args[3]
	if proto != "tcp" && proto != "udp" {
		fail(fmt.Errorf("protocol must be tcp or udp"))
	}
	size, count, rate := num(os.Args[4]), num(os.Args[5]), num(os.Args[6])
	if size < 8 || size > 65507 || count < 1 || count > 2000000 || rate < 0 {
		fail(fmt.Errorf("invalid size/count/rate"))
	}
	// One extra byte makes oversized UDP datagrams fail the size check
	// instead of silently accepting a truncated prefix.
	b := make([]byte, size+1)
	start := time.Now()
	var packets, bad, duplicate, outOfOrder int
	var bytes int64
	if mode == "send" {
		c, e := net.DialTimeout(proto, addr, 5*time.Second)
		fail(e)
		defer c.Close()
		fail(c.SetWriteDeadline(time.Now().Add(180 * time.Second)))
		start = time.Now()
		for i := 0; i < count; i++ {
			fill(b[:size], i)
			n, e := c.Write(b[:size])
			fail(e)
			if n != size {
				fail(io.ErrShortWrite)
			}
			packets++
			bytes += int64(n)
			if rate > 0 && i%32 == 31 {
				due := start.Add(time.Duration(int64(i+1) * int64(size) * 8 * 1000 / int64(rate)))
				if d := time.Until(due); d > 0 {
					time.Sleep(d)
				}
			}
		}
	} else if mode == "recv" {
		var c net.Conn
		if proto == "tcp" {
			l, e := net.Listen(proto, addr)
			fail(e)
			fail(l.(*net.TCPListener).SetDeadline(time.Now().Add(30 * time.Second)))
			fmt.Println("ready")
			c, e = l.Accept()
			fail(e)
			l.Close()
		} else {
			a, e := net.ResolveUDPAddr(proto, addr)
			fail(e)
			u, e := net.ListenUDP(proto, a)
			fail(e)
			raw, e := u.SyscallConn()
			fail(e)
			var se error
			fail(raw.Control(func(fd uintptr) {
				se = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_RCVBUFFORCE, 4<<20)
			}))
			fail(se)
			c = u
			fmt.Println("ready")
		}
		defer c.Close()
		seen := make([]bool, count)
		start = time.Now()
		fail(c.SetReadDeadline(time.Now().Add(30 * time.Second)))
		for packets < count {
			var n int
			var e error
			if proto == "tcp" {
				n, e = io.ReadFull(c, b[:size])
			} else {
				n, e = c.Read(b)
			}
			if e != nil {
				if e == io.EOF {
					break
				}
				if t, ok := e.(net.Error); ok && t.Timeout() {
					break
				}
				fail(e)
			}
			fail(c.SetReadDeadline(time.Now().Add(3 * time.Second)))
			packets++
			bytes += int64(n)
			if n != size {
				bad++
				continue
			}
			seq := binary.BigEndian.Uint64(b)
			if proto == "tcp" && seq != uint64(packets-1) {
				outOfOrder++
			}
			if seq >= uint64(count) {
				bad++
				continue
			}
			if seen[seq] {
				duplicate++
			}
			seen[seq] = true
			for i := 8; i < n; i++ {
				if b[i] != byte(seq*131+uint64(i)*17) {
					bad++
					break
				}
			}
		}
	} else {
		fail(fmt.Errorf("invalid mode"))
	}
	fail(json.NewEncoder(os.Stdout).Encode(map[string]interface{}{"mode": mode, "protocol": proto, "packets": packets, "bytes": bytes, "bad_payload": bad, "duplicates": duplicate, "out_of_order": outOfOrder, "expected": count, "seconds": time.Since(start).Seconds()}))
	if bad != 0 || duplicate != 0 || outOfOrder != 0 || (mode == "recv" && packets != count) {
		os.Exit(3)
	}
}
