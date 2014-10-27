DESCRIPTION
=======================
This is a Nand Flash Controller(NFC) simulation.   
Hope it could be helpful to make developing of new NFC driver easier.   



HOW TO TEST
=======================

* commands to load the mtd device module  
./load  

* commands to make a ubi fs on mtd device and mount to a directory  
sudo modprobe ubi mtd=0  
sudo ubimkvol /dev/ubi0 -N ubifs-vol -m  
sudo mkdir -p /mnt/ubifs  
sudo mount -t ubifs /dev/ubi0_0 /mnt/ubifs  

* commands to unload ubi modules  
sudo umount /dev/ubi0_0  
sudo rmmod ubifs  
sudo rmmod ubi  
sudo rmmod nfcsim.ko  

* commands to unload mtd device module  
./unload  

