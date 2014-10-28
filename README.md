DESCRIPTION
=======================
This is a Nand Flash Controller simulation (nfcsim).   
Hope it could be helpful to make developing of new NFC driver easier.   



HOW TO TEST
=======================
* build nfcsim source
make

* commands to load the nfcsim module  
./load  

* commands to make a ubi fs on nfcsim device  
sudo modprobe ubi mtd=0  
sudo ubimkvol /dev/ubi0 -N ubifs-vol -m  

* mount ubi device to a directory  
mkdir -p ~/mnt/ubifs  
sudo mount -t ubifs /dev/ubi0_0 ~/mnt/ubifs  
Now you can copy a file to test ERASE/PROGRAM, and read a file to test READ0.  


* commands to unload ubi modules  
sudo umount /dev/ubi0_0  
sudo rmmod ubifs  
sudo rmmod ubi  

* commands to unload nfcsim module  
./unload  

* to see the log output of the nfcsim module
tail -f /var/log/kern.log



CONFIG CHIP METADATA
=======================
To configure chip metadata like block size, page size, oob size etc.
You could:  
Option 1:     
Correctly set value of `NFC_FIRST_ID_BYTE, NFC_SECOND_ID_BYTE, NFC_THIRD_ID_BYTE, NFC_FOURTH_ID_BYTE`  
These value are used for READ ID command's response. NAND layer will decode the response to get the chip metadata info.  
You need to make sure you have encoded information correctly.   

Option 2:  
This is a simpler way, you can set the meta in variable nfc_meta_dev. NAND will match the `NFC_SECOND_ID_BYTE` value to `nfc_meta_dev.dev_id`, and will use the configuration in `nfc_meta_dev` instead of parsing meta from READ ID response.

WARNING:  
Please do not configure page size to more than 4096, the current implementation does not support that (due to hardcoded size of chip buffer). 



HOW SIMULATION WORKS
=======================
Page Simulation
----------
One page contains user data part and oob data part.  
oob part is saved behind user data part.

A page is allocated when it is firstly accessed (read of program). After allocation, all bits in the page will be set to 1. 

Pages where freed when block is erased.   

Memory simulate pages are managed via `nfcs_info.pages`. 


Chip Buffer Simulation
----------
A chip internal buffer is also simulated, mananged via `nfcs_info.buf`. 
All command handling involves data (like READ ID, READ0, PROGRAM ... etc) transfer will have data copied to internal chip buffer firstly.  

Then data in chip buffer will be copied to pages (like in case of PROGRAM), or copied to upper level buffer (like in case of READ0)   


Command Support
----------
Command handling where implemented in `nfc_cmdfunc`, it only supports minimal number of commands that are required to run a UBI filesystem.   
These commands are:  
```
NAND_CMD_READ0		0  
NAND_CMD_PAGEPROG	0x10  
NAND_CMD_READOOB	0x50  
NAND_CMD_ERASE1		0x60  
NAND_CMD_ERASE2		0xd0  
NAND_CMD_STATUS		0x70  
NAND_CMD_SEQIN		0x80  
NAND_CMD_READID		0x90  
NAND_CMD_RESET		0xff  
```








