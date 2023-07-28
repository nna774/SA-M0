package bp35a1

import (
	"bufio"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/tarm/serial"
)

type Client interface {
	SKInfo() (*Info, error)
	Auth(string, string) error
	Scan() (*Scan, error)
	SetChannel(string, string) error
	SKLL64(string) (string, error)
	SKJOIN(string) error

	ReadInstantaneousPower(string) (int, error) // TODO: addrを、引数ではなくclientの中に持たせる。
}

func NewClient(path string) (Client, error) {
	c := &serial.Config{
		Name:        path,
		Baud:        115200,
		ReadTimeout: 15 * time.Second,
		Size:        8,
		Parity:      serial.ParityNone,
	}
	s, err := serial.OpenPort(c)
	if err != nil {
		return nil, err
	}
	return &client{port: s}, nil
}

type Info struct {
	Value string // TODO:
}

type Scan struct {
	// ちゃんとintとかにしたほうがいい気もするけど、どうせstringで送るんだよな……。
	Channel     string
	ChannelPage string
	PanID       string
	Addr        string
	LQI         string
	PairID      string
}

type client struct {
	port *serial.Port
	s    *bufio.Scanner
}

func (c *client) send(cmd string) error {
	_, err := c.port.Write([]byte(cmd + "\r\n"))
	fmt.Printf("### send: %v\n", cmd)
	return err
}

func (c *client) readLine() string {
	if c.s == nil {
		c.s = bufio.NewScanner(c.port)
	}
	c.s.Scan()
	t := c.s.Text()
	fmt.Printf("### read: %v\n", t)
	return t
}

func (c *client) echobackOf(cmd string) error {
	if buf := c.readLine(); !strings.HasPrefix(buf, cmd) {
		return fmt.Errorf("got strange response: %v", buf)
	}
	return nil
}

func (c *client) expectOK() error {
	if buf := c.readLine(); buf != "OK" {
		return fmt.Errorf("not OK: %v", buf)
	}
	return nil
}

func (c *client) expectEvent(number string) error {
	if buf := c.readLine(); !strings.HasPrefix(buf, "EVENT "+number) {
		return fmt.Errorf("not EVENT %v: %v", number, buf)
	}
	return nil
}

func (c *client) SKInfo() (*Info, error) {
	if err := c.send("SKINFO"); err != nil {
		return nil, err
	}
	if err := c.echobackOf("SKINFO"); err != nil {
		return nil, err
	}

	buf := c.readLine()
	if len(buf) < 4 || buf[0:4] == "FAIL" {
		return nil, fmt.Errorf("got fail: %v", buf)
	}
	value := buf

	if err := c.expectOK(); err != nil {
		return nil, err
	}
	return &Info{Value: value}, nil
}

func (c *client) Auth(id string, pass string) error {
	if err := c.send("SKSETRBID " + id); err != nil {
		return err
	}
	if err := c.echobackOf("SKSETRBID"); err != nil {
		return err
	}
	if err := c.expectOK(); err != nil {
		return err
	}

	if err := c.send("SKSETPWD C " + pass); err != nil {
		return err
	}
	if err := c.echobackOf("SKSETPWD C"); err != nil {
		return err
	}
	if err := c.expectOK(); err != nil {
		return err
	}
	return nil
}

func cut(x string) string {
	_, ret, _ := strings.Cut(x, ":")
	return ret
}

func (c *client) Scan() (*Scan, error) {
	for i := 4; i < 10; i++ {
		if err := c.send(fmt.Sprintf("SKSCAN 2 FFFFFFFF %d", i)); err != nil {
			return nil, err
		}
		if err := c.echobackOf("SKSCAN"); err != nil {
			return nil, err
		}
		if err := c.expectOK(); err != nil {
			return nil, err
		}
		buf := c.readLine()
		if strings.HasPrefix(buf, "EVENT 22") {
			fmt.Printf("event: %v\n", buf)
		} else if strings.HasPrefix(buf, "EVENT 20") {
			// https://www.rohm.co.jp/products/wireless-communication/specified-low-power-radio-modules/bp35a1-product#designResources パスワードが必要(スタートアップマニュアルに書いてある)
			// みつかる度に20が返り、22が来ると終了。
			if buf := c.readLine(); buf != "EPANDESC" {
				return nil, fmt.Errorf("got strange response: %v", buf)
			}
			s := &Scan{}
			s.Channel = cut(c.readLine())
			s.ChannelPage = cut(c.readLine())
			s.PanID = cut(c.readLine())
			s.Addr = cut(c.readLine())
			s.LQI = cut(c.readLine())
			s.PairID = cut(c.readLine())

			if buf := c.readLine(); !strings.HasPrefix(buf, "EVENT 22") {
				return s, fmt.Errorf("got unknown event: %v", buf)
			}
			return s, nil
		} else {
			fmt.Printf("!! got %v(byte: %v)\n", buf, []byte(buf))
		}
		// time.Sleep((10*time.Duration(math.Pow(2, float64(i))) + 500) * time.Millisecond)
	}
	return nil, fmt.Errorf("not found")
}

func (c *client) SetChannel(channel string, panID string) error {
	if err := c.send("SKSREG S2 " + channel); err != nil {
		return err
	}
	if err := c.echobackOf("SKSREG"); err != nil {
		return err
	}
	if err := c.expectOK(); err != nil {
		return err
	}

	if err := c.send("SKSREG S3 " + panID); err != nil {
		return err
	}
	if err := c.echobackOf("SKSREG"); err != nil {
		return err
	}
	if err := c.expectOK(); err != nil {
		return err
	}

	return nil
}

func (c *client) SKLL64(addr string) (string, error) {
	if err := c.send("SKLL64 " + addr); err != nil {
		return "", err
	}
	if err := c.echobackOf("SKLL64"); err != nil {
		return "", err
	}
	return c.readLine(), nil
}

type udpPacket struct {
	Src       string
	Dst       string
	RPort     string // intにする？
	Lport     string
	SenderLLA string
	Secured   bool
	Len       uint16
	Data      string // 00000028A00000024A1F4DC76B6F34FC0007000000040000000000010002000000040000043A0004 40byte
}

func parseERXUDP(buf string) (*udpPacket, error) {
	seps := strings.SplitN(buf, " ", 9)
	if len(seps) != 9 || seps[0] != "ERXUDP" {
		return nil, fmt.Errorf("broken ERXUDP: %v", buf)
	}
	p := &udpPacket{}
	p.Src = seps[1]
	p.Dst = seps[2]
	p.RPort = seps[3]
	p.Lport = seps[4]
	p.SenderLLA = seps[5]
	p.Secured = seps[6] == "1"
	val, _ := strconv.ParseInt(seps[7], 16, 16)
	p.Len = uint16(val)
	p.Data = seps[8]
	return p, nil
}

func (c *client) SKJOIN(addr string) error {
	if err := c.send("SKJOIN " + addr); err != nil {
		return err
	}
	if err := c.echobackOf("SKJOIN"); err != nil {
		return err
	}
	if err := c.expectOK(); err != nil {
		return err
	}

	buf := ""
	for {
		buf = c.readLine()
		if strings.HasPrefix(buf, "EVENT 25") {
			fmt.Printf("SKJOIN success!!\n")
			return nil
		} else if strings.HasPrefix(buf, "EVENT 24") {
			fmt.Printf("SKJOIN failed!\n")
			return fmt.Errorf("SKJOIN failed")
		} else if strings.HasPrefix(buf, "EVENT 21") {
			fmt.Printf("SKJOIN packet sent\n")
		} else if strings.HasPrefix(buf, "EVENT 02") {
			fmt.Printf("NA reieved\n")
		} else if strings.HasPrefix(buf, "ERXUDP") {
			p, err := parseERXUDP(buf)
			if err != nil {
				return err
			}
			fmt.Printf("udp: %+v\n", p)
		}
	}
}

func (c *client) SKSENDTO(addr string, data []byte) error {
	buf := []byte("SKSENDTO 1 " + addr + " 0E1A 1 " + fmt.Sprintf("%04x", len(data)) + " ")
	buf = append(buf, data...)
	if _, err := c.port.Write(buf); err != nil {
		return err
	}
	if err := c.echobackOf("SKSENDTO"); err != nil {
		return err
	}
	if err := c.expectEvent("21"); err != nil {
		return err
	}
	if err := c.expectOK(); err != nil {
		return err
	}
	return nil
}

func parseDenbun(data string) { // TODO: 名前と型を考える。とりあえず全部画面に出す。
	fmt.Printf("EHD1: %v\n", data[0:2])
	fmt.Printf("EHD2: %v\n", data[2:4])
	fmt.Printf("TID: %v\n", data[4:8])
	fmt.Printf("SEOJ: %v\n", data[8:14])
	fmt.Printf("DEOJ: %v\n", data[14:20])
	fmt.Printf("ESV: %v\n", data[20:22])
	fmt.Printf("OPC: %v\n", data[22:24])
	if len(data) < 24 {
		return
	}
	fmt.Printf("EPC1: %v\n", data[24:26])
	fmt.Printf("PDC1: %v\n", data[26:28])
	fmt.Printf("leading: %v\n", data[28:])
}

func (c *client) ReadInstantaneousPower(addr string) (int, error) {
	req := []byte{0x10, 0x81, 0xDE, 0xAD, 0x05, 0xFF, 0x01, 0x02, 0x88, 0x01, 0x62, 0x01, 0xE7, 0x00} // TODO: なんとかする。
	err := c.SKSENDTO(addr, req)
	if err != nil {
		return 0, err
	}
	for {
		buf := c.readLine()
		if !strings.HasPrefix(buf, "ERXUDP") {
			return 0, fmt.Errorf("got strange response: %v", buf)
		}
		p, err := parseERXUDP(buf)
		if err != nil {
			return 0, err
		}
		fmt.Printf("udp: %+v\n", p)
		parseDenbun(p.Data)

		// TODO: なんとかしてくれ！！(parseDenbunをなんかうまいデータを返すようにすればよい)
		if p.Data[20:22] == "72" && p.Data[8:14] == "028801" {
			if p.Data[24:26] == "E7" {
				v, err := strconv.ParseInt(p.Data[28:36], 16, 16)
				if err != nil {
					return 0, err
				}
				return int(v), nil
			}
		}
	}
}

// See https://golang.org/doc/effective_go.html#blank_implements
var _ Client = (*client)(nil)
