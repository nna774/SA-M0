package bp35a1

import (
	"bufio"
	"fmt"
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

	return &client{
		port: s,
	}, nil
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
		}
	}

	return fmt.Errorf("got unknown event: %v", buf)
}

// See https://golang.org/doc/effective_go.html#blank_implements
var _ Client = (*client)(nil)
