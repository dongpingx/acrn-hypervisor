This directory contains the software that is necessary to run a real-time application between a real-time VM and a standard VM using the acrn-hypervisor.

The rtvm directory contains the code required to pipe data from cyclictest to the uservm using the inter-vm shared memory feature that acrn-hypervisor exposes to its VMs.

The uservm directory contains the code required to read the piped data from the rtvm, process the data, and display that data over a web application that can be accessed from the hypervisor's service VM.

To build and run the applications, copy this repo to your VMs, run make in the directory that corresponds to the VM that you are running, and then follow the sample app guide in the acrn-hypervisor documentation.