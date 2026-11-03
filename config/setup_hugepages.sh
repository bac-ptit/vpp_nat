#!/usr/bin/env bash
# config/setup_hugepages.sh
# Configures hugepages and prepares host environment for high-performance VPP DPDK data planes.

set -euo pipefail

HUGEPAGES_2M=${1:-4096} # Default: 4096 x 2MB = 8GB hugepages

echo "===================================================="
echo "  Setting up Hugepages and DPDK Environment for NAT "
echo "===================================================="

# Check root privileges
if [ "$EUID" -ne 0 ]; then
  echo "[-] Please run as root (or use sudo)."
  exit 1
fi

echo "[+] Allocating ${HUGEPAGES_2M} 2MB hugepages..."
echo "${HUGEPAGES_2M}" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Mount hugetlbfs if not already mounted
if ! mount | grep -q "/dev/hugepages"; then
  echo "[+] Mounting hugetlbfs at /dev/hugepages..."
  mkdir -p /dev/hugepages
  mount -t hugetlbfs nodev /dev/hugepages
fi

# Verify allocation
ALLOCATED=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
FREE=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
echo "[+] Total Hugepages: ${ALLOCATED}, Free Hugepages: ${FREE}"

# Load vfio-pci module
echo "[+] Loading vfio-pci kernel driver..."
modprobe vfio-pci || true

# Set permissions for non-root DPDK access if needed
chmod -R 777 /dev/hugepages

# Create runtime directories for VPP sockets
mkdir -p /run/vpp /var/log/vpp
chmod 777 /run/vpp /var/log/vpp

echo "[+] Hugepages and host environment setup successfully completed."
