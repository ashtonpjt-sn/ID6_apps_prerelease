package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"encoding/xml"
	"errors"
	"fmt"
	"io/fs"
	"net"
	"regexp"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
)

//go:embed payload
var embeddedPayload embed.FS

var bundled fs.FS = embeddedPayload

const (
	root        = "/var/opt/hmi/data/ID5"
	installLock = root + "/GaugeDeck-install-lock"
	fileOpsName = "gaugedeck-fileops"
)

type vehicleInput struct {
	Head, Gateway, Local, Version, Fingerprint, DisplayResolution, Password string
}

type inspection struct {
	Version, Result, Fingerprint, Display, Touch string
	Ready, CanRecover                            bool
	screen                                       panel
}

var firmwarePattern = regexp.MustCompile(`(?i)\bNBTevo_([a-z][0-9]{5}[a-z])\b`)

func firmwareName(text string) string {
	m := firmwarePattern.FindStringSubmatch(text)
	if len(m) == 2 {
		return "NBTevo_" + strings.ToUpper(m[1])
	}
	return ""
}

func sshConnect(in vehicleInput) (*ssh.Client, net.Conn, string, error) {
	config := &ssh.ClientConfig{
		User: "root",
		Config: ssh.Config{
			KeyExchanges: append(append([]string{}, ssh.SupportedAlgorithms().KeyExchanges...),
				"diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1"),
			Ciphers: append(append([]string{}, ssh.SupportedAlgorithms().Ciphers...), "aes128-cbc", "aes256-cbc", "3des-cbc"),
			MACs:    append(append([]string{}, ssh.SupportedAlgorithms().MACs...), "hmac-sha1", "hmac-sha1-96"),
		},
		HostKeyAlgorithms: append(append([]string{}, ssh.SupportedAlgorithms().HostKeys...), "ssh-rsa", "ssh-dss"),
	}
	if in.Fingerprint != "" {
		if in.Password == "" {
			return nil, nil, "", errors.New("Enter the root SSH password for the connected head unit.")
		}
		config.Auth = []ssh.AuthMethod{ssh.Password(in.Password)}
	}
	var observed string
	config.HostKeyCallback = func(_ string, _ net.Addr, key ssh.PublicKey) error {
		observed = ssh.FingerprintSHA256(key)
		if observed != in.Fingerprint {
			return errors.New("SSH fingerprint has not been confirmed")
		}
		return nil
	}
	c, err := enetDial(in.Local, in.Head, "22", 4*time.Second)
	if err != nil {
		return nil, nil, "", fmt.Errorf("could not reach head unit SSH: %w", err)
	}
	_ = c.SetDeadline(time.Now().Add(8 * time.Second))
	conn, channels, requests, err := ssh.NewClientConn(c, net.JoinHostPort(in.Head, "22"), config)
	if err != nil {
		c.Close()
		return nil, nil, observed, err
	}
	return ssh.NewClient(conn, channels, requests), c, observed, nil
}

type remote struct {
	client *ssh.Client
	socket net.Conn
}

type bounded struct {
	data  bytes.Buffer
	limit int
}

func (b *bounded) Write(p []byte) (int, error) {
	if b.data.Len()+len(p) > b.limit {
		return 0, errors.New("SSH response exceeded expected size")
	}
	return b.data.Write(p)
}

func (r remote) run(cmd string, input []byte, limit int) ([]byte, error) {
	_ = r.socket.SetDeadline(time.Now().Add(8 * time.Second))
	s, err := r.client.NewSession()
	if err != nil {
		return nil, err
	}
	defer s.Close()
	out, stderr := bounded{limit: limit}, bounded{limit: 2048}
	s.Stdin = bytes.NewReader(input)
	s.Stdout, s.Stderr = &out, &stderr
	if err = s.Run(cmd); err != nil {
		if detail := strings.TrimSpace(stderr.data.String()); detail != "" {
			return nil, fmt.Errorf("%w: %s", err, detail)
		}
		return nil, err
	}
	return out.data.Bytes(), nil
}

const appFramework = "libAppFramework.A.so"
const coreFramework = "libCoreFramework.A.so"

type frameworkPair struct{ app, core string }

var profiles = map[string]frameworkPair{
	"NBTevo_N18092I": {
		"93f4c7ab7e59c0adf7923b9b491de06165ba9d97bbe7412292e7ddc418ca2123",
		"64ba154c68c772f9ca779fc18507f5b34715be58ded4a198b8a62b90d16c3ee0",
	},
	"NBTevo_V20153I": {
		"085b7f2a9f96734be51621a70a04c24d01525eb5e9852f45c845a7d2f4ae4c67",
		"56705e56c349d9ae57a7782bd39b795cfc01391ba8ff155046f32fbe8ffad2cb",
	},
	"NBTevo_Y24073J": {
		"ef2488b007d592301d8f21d9aa60771ad552d06b5e97c29e7813e3946644d820",
		"cb0e488d1ebe2e30de87bb56f0e93d6a2d76b44309b0b47fef5b0b7cec048973",
	},
}

type remoteRunner interface {
	run(cmd string, input []byte, limit int) ([]byte, error)
}

func inspect(r remoteRunner, in vehicleInput, gatewayCheck func(string, string) (string, error)) inspection {
	result := inspection{Fingerprint: in.Fingerprint}
	versionFile, _ := r.run("cat /opt/sys/etc/nbt_version.txt", nil, 1024)
	detected := firmwareName(string(versionFile))
	entered := firmwareName(in.Version)
	if in.Version != "" && !strings.EqualFold(in.Version, entered) {
		result.Result = "Enter the full NBTevo code shown on iDrive."
		return result
	}
	if detected != "" && entered != "" && entered != detected {
		result.Version = detected
		result.Result = "The entered version differs from the version reported over SSH."
		return result
	}
	result.Version = detected
	if result.Version == "" {
		result.Version = entered
	}
	if result.Version == "" {
		result.Result = "The head unit did not expose the screen version over SSH. Enter the full code shown in iDrive and check again."
		return result
	}
	profile, ok := profiles[result.Version]
	if !ok {
		result.Result = "This firmware is outside this three-car preview. Installation is disabled."
		return result
	}
	info, err := r.run("uname -a", nil, 1024)
	platform := strings.Fields(string(info))
	if err != nil || len(platform) < 4 || platform[0] != "QNX" || platform[2] != "6.5.0" || platform[len(platform)-1] != "armle" {
		result.Result = "The head unit must report QNX 6.5.0 ARM."
		return result
	}
	for _, framework := range []struct{ name, hash string }{{appFramework, profile.app}, {coreFramework, profile.core}} {
		raw, err := r.run("cat /opt/hmi/ID5/lib/"+framework.name, nil, 4<<20)
		if err != nil {
			result.Result = "Could not read " + framework.name + " over SSH. Check the head-unit IP and temporary SSH access."
			return result
		}
		if digest(raw) != framework.hash {
			result.Result = "The on-car framework fingerprint for " + framework.name + " does not match the supplied capture. Check the head-unit IP. If it is correct, see the source repository’s compatibility workflow."
			return result
		}
	}
	display, err := r.run("/opt/sys/bin/LayerManagerControl get screen 0", nil, 8192)
	if err == nil {
		result.screen = readPanel(display)
	}
	result.Display = result.screen.String()
	if in.DisplayResolution != "" {
		selected := manualPanel(in.DisplayResolution)
		if !selected.supported() {
			result.Result = "Choose a supported manual resolution. Custom layouts require 800–1920 pixels wide, 480–720 high, and an aspect ratio between 5:3 and 3:1."
			return result
		}
		if result.screen != (panel{}) && result.screen != selected {
			result.Result = "The manual resolution differs from the active iDrive resolution. Choose the detected resolution or use Automatic."
			return result
		}
		result.screen = selected
		result.Display = result.screen.String() + " (manual selection)"
		if !selected.automatic() {
			result.Display += " · Experimental layout"
		}
	} else if !result.screen.automatic() {
		result.Result = "Choose the known iDrive resolution manually, then check compatibility again."
		return result
	}
	coding, _ := r.run(`for f in /var/opt/conn/carplay/carplay.ini /fs/sda1/opt/conn/carplay/carplay.ini; do if test -r "$f"; then sed -n '/^#Coded is:/p' "$f"; break; fi; done`, nil, 1024)
	result.Touch = "Touch coding unknown. Use the iDrive controller to navigate."
	if touchCoding.Match(coding) {
		result.Touch = "Touch coded for factory CarPlay. Tap behavior in the gauge menus needs a parked check."
	}
	if _, err := usableIP(in.Gateway); err != nil || in.Gateway == in.Head {
		result.Result = "Enter separate head unit and diagnostic gateway ENET addresses."
		return result
	}
	hu, err := gatewayCheck(in.Local, in.Gateway)
	if err != nil {
		result.Result = "Could not reach the diagnostic gateway on port 6801. Check the gateway IP and ENET connection."
		return result
	}
	if hu != "" && hu != in.Head {
		result.Result = "The gateway reports a different head unit IP. Check the ENET connection."
		return result
	}
	if _, err := r.run("test -d "+root+"/Plugins && test -w "+root+"/Plugins && test -w "+root, nil, 128); err != nil {
		result.Result = "The expected writable ID5 Plugins folder is unavailable."
		return result
	}
	result.Result = "Frameworks matched. BoostDeck, ShiftDeck, MHD Maps, Warm-Up, Performance Timer and Fault Scanner are ready for a parked preview."
	result.Ready = true
	return result
}

func digest(b []byte) string { sum := sha256.Sum256(b); return hex.EncodeToString(sum[:]) }

type plugin struct{ name, worker, connector string }

var apps = []plugin{
	{"BoostDeck", "boostdeck-collector-qnx", "100"},
	{"ShiftDeck", "shift-deck-qnx", "104"},
	{"MhdMaps", "mhd-maps-qnx", "103"},
	{"WarmUp", "warm-up-qnx", "105"},
	{"PerfTimer", "perf-timer-qnx", "106"},
	{"FaultScan", "fault-scan-qnx", "107"},
}

type manifest struct {
	Name      string `xml:"name"`
	Connector string `xml:"config>dependencies>pluginAppConnector>id"`
}

var pluginPath = regexp.MustCompile(`^/var/opt/hmi/data/ID5/Plugins/[A-Za-z0-9_.+/-]+\.manifest$`)

func checkConnectors(r remoteRunner) error {
	list, err := r.run("find "+root+"/Plugins -name '*.manifest' -type f", nil, 32768)
	if err != nil {
		return fmt.Errorf("could not inspect existing plugins: %w", err)
	}
	for _, path := range strings.Fields(string(list)) {
		if !pluginPath.MatchString(path) || strings.Contains(path, "..") {
			return errors.New("unexpected existing plugin path")
		}
		raw, err := r.run("cat "+path, nil, 65536)
		if err != nil {
			return err
		}
		var m manifest
		if err = xml.Unmarshal(raw, &m); err != nil {
			return fmt.Errorf("could not validate existing manifest %s", path)
		}
		for _, app := range apps {
			if strings.TrimSpace(m.Connector) == app.connector && (m.Name != app.name || path != root+"/Plugins/"+app.name+"/"+app.name+".manifest") {
				return fmt.Errorf("connector %s is already used by %s", app.connector, path)
			}
		}
	}
	return nil
}

type blockedInstall struct {
	report     string
	canRecover bool
}

func (e *blockedInstall) Error() string {
	return "The install lock already exists.\n\n" + e.report
}

func install(r remoteRunner, gateway string, screen panel, recoverLock bool) (string, error) {
	if _, err := usableIP(gateway); err != nil {
		return "", err
	}
	if !screen.supported() {
		return "", errors.New("no supported display layout was selected")
	}
	lockCheck := "if test -e " + installLock + " || test -L " + installLock + "; then echo locked; fi"
	lock, err := r.run(lockCheck, nil, 512)
	if err != nil {
		return "", fmt.Errorf("install lock check failed: %w", err)
	}
	locked := strings.TrimSpace(string(lock)) == "locked"
	if locked {
		state, report, err := inspectInstallLock(r, "")
		if err != nil {
			return "", fmt.Errorf("could not inspect the existing install lock: %w", err)
		}
		if !recoverLock || state != "staged-only" {
			return "", &blockedInstall{report: report, canRecover: state == "staged-only"}
		}
	} else if len(bytes.TrimSpace(lock)) != 0 {
		return "", errors.New("unexpected response from the install-lock check")
	}
	if err := checkConnectors(r); err != nil {
		return "", err
	}
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return "", err
	}
	tx := root + "/GaugeDeck-" + hex.EncodeToString(nonce[:])
	if _, err := r.run("umask 077; mkdir "+tx+" && chmod 755 "+tx, nil, 512); err != nil {
		return "", err
	}
	if err := stageFileOps(r, tx); err != nil {
		return tx, err
	}
	if locked {
		state, report, err := inspectInstallLock(r, tx)
		if err != nil {
			return tx, fmt.Errorf("could not recover the existing install lock: %w", err)
		}
		if state != "cleared" {
			return tx, &blockedInstall{report: report}
		}
	}
	verifyFiles := make(map[string][]byte)
	for _, app := range apps {
		stage := tx + "/new-" + app.name
		live := root + "/Plugins/" + app.name
		check := "test ! -L " + live + " && { test ! -e " + live + " || { test -d " + live + " && test -f " + live + "/" + app.name + ".manifest; }; }"
		if _, err := r.run(check+" && umask 022 && mkdir "+stage, nil, 512); err != nil {
			return tx, fmt.Errorf("could not prepare %s staging directory: %w", app.name, err)
		}
		files, err := fs.ReadDir(bundled, "payload/"+app.name)
		if err != nil {
			return tx, err
		}
		filesData := map[string][]byte{"gateway-ip.txt": []byte(gateway + "\n")}
		for _, file := range files {
			name := file.Name()
			switch name {
			case app.name + ".manifest", app.name + ".xml", app.name + "_ALL_Images.zip", "lib" + app.name + ".so", app.worker:
			default:
				return tx, errors.New("unexpected file in embedded payload")
			}
			data, err := fs.ReadFile(bundled, "payload/"+app.name+"/"+name)
			if err != nil {
				return tx, err
			}
			if name == app.name+".xml" {
				data = scaleLayout(data, app.name, screen)
			}
			filesData[name] = data
		}
		if len(filesData) != 6 {
			return tx, errors.New("embedded payload is incomplete")
		}
		for name, data := range filesData {
			path := stage + "/" + name
			if _, err := r.run("umask 022; set -C; cat > "+path, data, 512); err != nil {
				return tx, fmt.Errorf("could not upload %s: %w", name, err)
			}
			read, err := r.run("cat "+path, nil, len(data)+1)
			if err != nil || !bytes.Equal(read, data) {
				return tx, fmt.Errorf("upload verification failed for %s", name)
			}
			verifyFiles[live+"/"+name] = data
		}
		if _, err := r.run("chmod 755 "+stage+"/"+app.worker, nil, 512); err != nil {
			return tx, fmt.Errorf("could not set execute permission on %s: %w", app.worker, err)
		}
	}
	// Keep the lock and rollback in one QNX shell. Pass the script over stdin
	// so SSH only has to execute a short, fixed command.
	if _, err := r.run("/bin/sh -s", publicationScript(tx), 1024); err != nil {
		return tx, fmt.Errorf("publication was interrupted: %w", err)
	}
	for path, expected := range verifyFiles {
		actual, err := r.run("cat "+path, nil, len(expected)+1)
		if err != nil {
			return tx, fmt.Errorf("could not read back published file %s: %w", path, err)
		}
		if !bytes.Equal(actual, expected) {
			return tx, fmt.Errorf("published file differs from staged data: %s", path)
		}
	}
	return tx, nil
}

func stageFileOps(r remoteRunner, tx string) error {
	data, err := fs.ReadFile(bundled, "payload/tools/"+fileOpsName)
	if err != nil {
		return err
	}
	path := tx + "/" + fileOpsName
	if _, err := r.run("umask 077; set -C; cat > "+path, data, 512); err != nil {
		return fmt.Errorf("could not upload the file operations helper: %w", err)
	}
	actual, err := r.run("cat "+path, nil, len(data)+1)
	if err != nil {
		return fmt.Errorf("could not read back the file operations helper: %w", err)
	}
	if !bytes.Equal(actual, data) {
		return errors.New("file operations helper differs from the bundled copy")
	}
	if _, err := r.run("chmod 700 "+path+" && "+path+" check", nil, 512); err != nil {
		return fmt.Errorf("the head unit could not run the bundled file operations helper: %w", err)
	}
	return nil
}

func inspectInstallLock(r remoteRunner, recoveryTx string) (string, string, error) {
	var script bytes.Buffer
	fmt.Fprintf(&script, "root=%s\nlock=%s\ncurrent=%s\nops=\"$current/%s\"\napps='", root, installLock, recoveryTx, fileOpsName)
	for _, app := range apps {
		fmt.Fprintf(&script, "%s ", app.name)
	}
	script.WriteString(`'
exists() { test -e "$1" || test -L "$1"; }
lock_empty() {
    for member in "$lock"/* "$lock"/.[!.]* "$lock"/..?*; do
        if exists "$member"; then return 1; fi
    done
    return 0
}
staged_only() {
    test -d "$lock" && test ! -L "$lock" && lock_empty || return 1
    for app in $apps; do
        if exists "$root/Plugins/$app"; then return 1; fi
    done
    complete=0
    for tx in "$root"/GaugeDeck-????????????????; do
        if test "$tx" = "$current" || ! exists "$tx"; then continue; fi
        test -d "$tx" && test ! -L "$tx" || return 1
        has_all_stages=1
        for app in $apps; do
            if exists "$tx/old-$app" || exists "$tx/fresh-$app"; then return 1; fi
            if ! test -d "$tx/new-$app" || test -L "$tx/new-$app"; then has_all_stages=0; fi
        done
        if test "$has_all_stages" = 1; then complete=1; fi
    done
    test "$complete" = 1
}
if staged_only; then
    if test -n "$current"; then
        "$ops" unlock "$lock" || exit 1
        echo cleared
        exit 0
    fi
    echo staged-only
else
    echo review
fi
show_path() {
    if test -L "$1"; then state=symlink
    elif test -d "$1"; then state=directory
    elif test -f "$1"; then state=file
    elif test -e "$1"; then state=other
    else state=missing
    fi
    echo "$2: $state"
}
echo "Install lock: $lock"
show_path "$lock" 'Lock type'
if test -d "$lock" && test ! -L "$lock"; then
    if lock_empty; then echo 'Lock contents: empty'; else echo 'Lock contents: nonempty'; fi
fi
echo
echo 'Live app paths:'
for app in $apps; do
    show_path "$root/Plugins/$app" "$app"
done
for tx in "$root"/GaugeDeck-????????????????; do
    if test ! -d "$tx" || test -L "$tx"; then continue; fi
    echo
    echo "Transaction $tx"
    for app in $apps; do
        for prefix in new old fresh; do
            marker="$prefix-$app"
            show_path "$tx/$marker" "$marker"
        done
    done
done
`)
	out, err := r.run("/bin/sh -s", script.Bytes(), 16384)
	if err != nil {
		return "", "", err
	}
	state, report, _ := strings.Cut(string(out), "\n")
	switch state {
	case "cleared", "staged-only", "review":
		return state, strings.TrimSpace(report), nil
	default:
		return "", "", errors.New("unexpected install recovery response")
	}
}

func publicationScript(tx string) []byte {
	var script bytes.Buffer
	names := make([]string, 0, len(apps))
	for _, app := range apps {
		names = append(names, app.name+":"+app.worker)
	}
	fmt.Fprintf(&script, "root=%s\ntx=%s\nops=\"$tx/%s\"\napps='%s'\n", root, tx, fileOpsName, strings.Join(names, " "))
	script.WriteString(`lock="$root/GaugeDeck-install-lock"
fail() { echo "$1" >&2; exit 1; }
paths() {
    live="$root/Plugins/$app"
    old="$tx/old-$app"
    stage="$tx/new-$app"
    fresh="$tx/fresh-$app"
}
cleanup() {
    if test "$locked" = 0; then return; fi
    rollback_failed=0
    if test "$done_ok" = 0; then
        for entry in $apps; do
	            app=${entry%%:*}
            paths
            if test -d "$old" || test -f "$fresh"; then
                if test ! -e "$stage" && test -d "$live"; then
                    "$ops" move "$live" "$stage" || rollback_failed=1
                fi
                if test -d "$old"; then
                    if test -e "$live" || test -L "$live"; then
                        rollback_failed=1
                    else
                        "$ops" move "$old" "$live" || rollback_failed=1
                    fi
                fi
            fi
        done
    fi
    if test "$rollback_failed" = 1; then
        echo 'Rollback needs inspection. The install lock and backups were kept.' >&2
    else
        if ! "$ops" unlock "$lock"; then
            trap - 0
            fail 'Could not release the install lock.'
        fi
    fi
}
locked=0
done_ok=0
trap cleanup 0
trap 'fail "Publication received a stop signal."' HUP INT TERM
if ! mkdir "$lock"; then
    if test -e "$lock" || test -L "$lock"; then
        fail 'The install lock already exists. Check for an active or interrupted install before retrying.'
    fi
    fail 'Could not create the install lock.'
fi
locked=1
for entry in $apps; do
    app=${entry%%:*}
    worker=${entry#*:}
    paths
    test ! -L "$live" || fail "$app plugin path is a symbolic link."
    if test -e "$live"; then
        test -d "$live" && test -f "$live/$app.manifest" || fail "$app plugin path cannot be replaced."
        "$ops" move "$live" "$old" || fail "Could not back up $app."
    else
        : > "$fresh" || fail "Could not record the new $app install."
    fi
    "$ops" move "$stage" "$live" || fail "Could not publish $app."
    test -f "$live/$app.manifest" && test -x "$live/$worker" || fail "Published $app is missing its manifest or executable worker."
done
done_ok=1
`)
	return script.Bytes()
}
