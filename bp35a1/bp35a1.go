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
}

func NewClient(path string) (Client, error) {
	c := &serial.Config{
		Name:        path,
		Baud:        115200,
		ReadTimeout: 5 * time.Second,
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
	Value string
}

type client struct {
	port *serial.Port
	s    *bufio.Scanner
}

func (c *client) send(cmd string) error {
	_, err := c.port.Write([]byte(cmd + "\r\n"))
	return err
}

func (c *client) readLine() string {
	if c.s == nil {
		c.s = bufio.NewScanner(c.port)
	}
	c.s.Scan()
	return c.s.Text()
}

func (c *client) SKInfo() (*Info, error) {
	err := c.send("SKINFO")
	if err != nil {
		return nil, err
	}
	buf := c.readLine()
	if buf != "SKINFO" {
		return nil, fmt.Errorf("got strange response: %v", buf)
	}

	buf = c.readLine()
	if len(buf) < 4 || buf[0:4] == "FAIL" {
		return nil, fmt.Errorf("got fail: %v", buf)
	}
	value := buf

	buf = c.readLine()
	if buf != "OK" {
		return nil, fmt.Errorf("not OK: %v, %v", value, buf)
	}
	return &Info{Value: value}, nil
}

func (c *client) Auth(id string, pass string) error {
	err := c.send("SKSETRBID " + id)
	if err != nil {
		return err
	}
	buf := c.readLine()
	if !strings.HasPrefix(buf, "SKSETRBID ") {
		return fmt.Errorf("got strange response: %v", buf)
	}
	buf = c.readLine()
	if buf != "OK" {
		return fmt.Errorf("not OK: %v", buf)
	}

	err = c.send("SKSETPWD C " + id)
	if err != nil {
		return err
	}
	buf = c.readLine()
	if !strings.HasPrefix(buf, "SKSETPWD C ") {
		return fmt.Errorf("got strange response: %v", buf)
	}
	buf = c.readLine()
	if buf != "OK" {
		return fmt.Errorf("not OK: %v", buf)
	}
	return nil
}

// See https://golang.org/doc/effective_go.html#blank_implements
var _ Client = (*client)(nil)
