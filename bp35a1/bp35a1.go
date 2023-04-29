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
}

func NewClient(path string) (Client, error) {
	c := &serial.Config{
		Name:        path,
		Baud:        115200,
		ReadTimeout: 10 * time.Second,
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
	Value string // TODO:
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
	buf := c.readLine()
	if !strings.HasPrefix(buf, cmd) {
		return fmt.Errorf("got strange response: %v", buf)
	}
	return nil
}

func (c *client) expectOK() error {
	buf := c.readLine()
	if buf != "OK" {
		return fmt.Errorf("not OK: %v", buf)
	}
	return nil
}

func (c *client) SKInfo() (*Info, error) {
	err := c.send("SKINFO")
	if err != nil {
		return nil, err
	}
	err = c.echobackOf("SKINFO")
	if err != nil {
		return nil, err
	}

	buf := c.readLine()
	if len(buf) < 4 || buf[0:4] == "FAIL" {
		return nil, fmt.Errorf("got fail: %v", buf)
	}
	value := buf

	err = c.expectOK()
	if err != nil {
		return nil, err
	}
	return &Info{Value: value}, nil
}

func (c *client) Auth(id string, pass string) error {
	err := c.send("SKSETRBID " + id)
	if err != nil {
		return err
	}
	err = c.echobackOf("SKSETRBID")
	if err != nil {
		return err
	}
	err = c.expectOK()
	if err != nil {
		return err
	}

	err = c.send("SKSETPWD C " + pass)
	if err != nil {
		return err
	}
	err = c.echobackOf("SKSETPWD C")
	if err != nil {
		return err
	}
	err = c.expectOK()
	if err != nil {
		return err
	}
	return nil
}

func (c *client) Scan() (*Scan, error) {
	for i := 4; i < 10; i++ {
		err := c.send(fmt.Sprintf("SKSCAN 2 FFFFFFFF %d", i))
		if err != nil {
			return nil, err
		}
		err = c.echobackOf("SKSCAN")
		if err != nil {
			return nil, err
		}
		err = c.expectOK()
		if err != nil {
			return nil, err
		}
		buf := c.readLine()
		if strings.HasPrefix(buf, "EVENT 22") {
			fmt.Printf("event: %v\n", buf)
		} else if strings.HasPrefix(buf, "EVENT 20") {
			// イベントの番号はわからない
			// https://www.rohm.co.jp/products/wireless-communication/specified-low-power-radio-modules/bp35a1-product#designResources パスワードが必要
			// が、22だとみつからない時で、20だとあった時なのでは？
			buf := c.readLine()
			fmt.Printf("%v\n", buf) // believe this is EPANDESC
			fmt.Printf("%v\n", c.readLine()) // Channel
			fmt.Printf("%v\n", c.readLine()) // Channel Page
			fmt.Printf("%v\n", c.readLine()) // Pan ID
			fmt.Printf("%v\n", c.readLine()) // Addr
			fmt.Printf("%v\n", c.readLine()) // LQI
			fmt.Printf("%v\n", c.readLine()) // PairID
			break
		} else {
			fmt.Printf("!! got %v(byte: %v)\n", buf, []byte(buf))
		}
		// time.Sleep((10*time.Duration(math.Pow(2, float64(i))) + 500) * time.Millisecond)
	}
	return nil, nil
}

// See https://golang.org/doc/effective_go.html#blank_implements
var _ Client = (*client)(nil)
