# x6100_trx for OpenX6X00

`x6100_trx` is a front end for the Xiegu X6100 on the stock baseband firmware (BASE
1.1.9), built on libx6100. This archive puts it and its files on a radio that runs an
OpenX6X00 image, from a Linux PC, in one step.

Source, documentation and issues: https://github.com/roelandjansen/libx6100

## What is in it

| Path in `payload/` | On the radio | What |
|---|---|---|
| `root/x6100_trx` | `/root/x6100_trx` | the program (static, no libraries needed) |
| `root/x6100_ui.sh` | `/root/x6100_ui.sh` | GUI off / on, boot choice, wfserver, `trx stop/start` |
| `root/x6100_giveaway.sh` | `/root/x6100_giveaway.sh` | clean the radio before giving it away |
| `root/x6100_trx_quickref.md` | `/root/x6100_trx_quickref.md` | keys, panels, console commands |
| `root/x6100_dist/` | `/root/x6100_dist/` | the examples and the install step |
| `etc/init.d/S96x6100_trx` | `/etc/init.d/S96x6100_trx` | starts x6100_trx at boot, only after `x6100_ui.sh boot trx` |
| `opt/wfserver/` | `/opt/wfserver/` | wfserver for wfview, with X6100.rig (only in archives built with it) |

## First: your ssh key on the radio

OpenX6X00 lets root in with a key only, never with a password (`ssh` says
`Permission denied (publickey)`). Once, on the PC:

```
ssh-keygen -t ed25519           # Enter for the defaults; skip when you have a key
cat ~/.ssh/id_ed25519.pub       # the one line to copy
```

Then in a browser `http://<address of the radio>/ssh`, paste that line under "Add a
key" and press Add. `ssh root@<address of the radio>` works from then on. The radio
keeps the key in its backup on the card's data partition, so it survives a new image
written over the old one.

## Install

On the PC (Linux, with `ssh` and GNU `tar`):

```
tar -xzf x6100_trx-<version>.tar.gz
cd x6100_trx-<version>
./install.sh 192.168.1.61
```

Use the radio's own address (shown by OpenX6X00, or your router), or change
`C_DEFAULT_HOST` at the top of `install.sh`. It uses your ssh key (above): one
connection for everything.

The install step on the radio:

- sets owner and modes of the installed files;
- creates `/etc/x6100` and copies `x6100_trx.conf`, `memories.conf` and
  `messages.conf` (and `wfserver.ini` with wfserver) from the examples, only where
  that file is missing: settings already on the radio are kept;
- installs no `remote.conf` (it holds passwords) and does not change what starts at
  boot.

Installing again (a newer version) is the same command. Stop a running x6100_trx first
(`/root/x6100_ui.sh trx stop`): a running program cannot be replaced.

## First start

On the radio (ssh, or the console):

```
/root/x6100_ui.sh stop       # the OpenX6X00 GUI off
/root/x6100_trx              # x6100_trx; PWR or Ctrl-C quits, the GUI comes back with x6100_ui.sh start
```

At every boot instead of the GUI: `/root/x6100_ui.sh boot trx` (`boot gui` undoes it).

## Remote rigs (optional)

The X6100 can be a control head for networked Icom rigs (IC-7851, IC-9700, IC-7760, ...):

```
cp /root/x6100_dist/x6100_remote.conf.example /etc/x6100/remote.conf
vi /etc/x6100/remote.conf
chmod 600 /etc/x6100/remote.conf
```

Then hold GEN for 1.5 s for the RIG panel. Details in the quick reference.
