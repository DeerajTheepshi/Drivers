#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb/input.h>
#include <linux/hid.h>

//All Linux Module need this
MODULE_AUTHOR("deeraj");
MODULE_DESCRIPTION("diprivi_mouse");
MODULE_LICENSE("GPL");

struct diprivi_mouse {
	char name[128];
	char phys[64];
	struct usb_device *usbdev;
	struct input_dev *indev;
	struct urb *irq;

	signed char *data;
	dma_addr_t data_dma;
};

static void diprivi_mouse_irq(struct urb *urb)
{
	//Get back the device related to the URB (Mentioned while filling the URB)
	struct diprivi_mouse *mouse = urb->context;
	signed char *data = mouse->data;
	struct input_dev *dev = mouse->indev;

	//This is when the URB is cancelled in Sync or Async, either ways, you try to 
	//resubmit the URB.
	if(urb->status == -ECONNRESET || urb->status == -ENOENT){
		int status = usb_submit_urb (urb, GFP_ATOMIC);
		if (status)
			printk(KERN_WARNING "USB - CANT SUBMIT");
 	} else if(urb->status == -ESHUTDOWN) return;

	//Log the key and send the information to input_core
	printk(KERN_WARNING "Interrupted, %d", data[1]);
	input_report_key(dev, BTN_LEFT,  data[1]==1);

	//I dont' actually need this now, but when multiple instructions are there, This line
	// Prevents buffering and exectues them all
	input_sync(dev);

	//Resubmit the URB
	int status = usb_submit_urb (urb, GFP_ATOMIC);
	if (status)
		printk(KERN_WARNING "Cant Submit URB");
}

static int diprivi_mouse_open(struct input_dev *dev)
{
	struct diprivi_mouse *mouse = input_get_drvdata(dev);

	mouse->irq->dev = mouse->usbdev;
	if (usb_submit_urb(mouse->irq, GFP_KERNEL))
		return -EIO;

	return 0;
}

static void diprivi_mouse_close(struct input_dev *dev)
{
	struct diprivi_mouse *mouse = input_get_drvdata(dev);

	usb_kill_urb(mouse->irq);
}

static int diprivi_mouse_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	//The moment USB core finds your device, it does a probe on the driver. The driver should 
	// check the device and accept the device if it can handle it.
	printk(KERN_WARNING "Reaching Probe");

	//Get the usb_device struc for the corresponding interface
	struct usb_device *dev_core = interface_to_usbdev(intf);

	//Pointer to the interface we are targetting
	struct usb_host_interface *interface;

	//Pointer to the enpoint of the interface we are working with
	struct usb_endpoint_descriptor *endpoint;

	//Your custom made struct for the driver
	struct diprivi_mouse *mouse;

	//Yedhukku (You can directly use it on mouse though)
	struct input_dev *input_dev;

	//Get the current setting for the interface
	interface = intf->cur_altsetting;

	//Im expecting the endpoints to be only one (IN ENDPOINT)
	if (interface->desc.bNumEndpoints != 1)
		return -ENODEV;

	//Get your target endpoint
	endpoint = &interface->endpoint[0].desc;

	//Check if its is INTERRUPT_IN endpoint (Mouse bhai, has to be int_in)
	if (!usb_endpoint_is_int_in(endpoint))
		return -ENODEV;

	//URB Required 2 things, 1)The endpoint, 2)The pipe to communicate
	int pipe = usb_rcvintpipe(dev_core, endpoint->bEndpointAddress);
	int maxp = usb_maxpacket(dev_core, pipe, usb_pipeout(pipe));

	//Allocate memory to mouse struct (More on this in references)
	mouse = kzalloc(sizeof(struct diprivi_mouse), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!mouse || !input_dev){
		input_free_device(input_dev);
		kfree(mouse);		
		return -ENOMEM;		
	}
		
	//DMA allocation for the target buffer. DMA is faster than CPU based access
	//Experimentation code is nodma_test.c
	mouse->data = usb_alloc_coherent(dev_core, 8, GFP_ATOMIC, &mouse->data_dma);
	if (!mouse->data){
		input_free_device(input_dev);
		kfree(mouse);		
		return -ENOMEM;		
	}

	//Create the URB object, Provide the memory flag which should be used.
	//Check out the general rule for memory allocation in references
	mouse->irq = usb_alloc_urb(0, GFP_KERNEL);
	if (!mouse->irq){
		usb_free_coherent(dev_core, 8, mouse->data, mouse->data_dma);
		input_free_device(input_dev);
		kfree(mouse);
		return -ENOMEM;
	}

	//Assign the created input and device to the mouse struct		
	mouse->usbdev = dev_core;
	mouse->indev = input_dev;

	//Idhuvum theva illa, but yet copy paste : Just so that the device property is set in the 
	//Sysfs properly
	if (dev_core->manufacturer)
		strlcpy(mouse->name, dev_core->manufacturer, sizeof(mouse->name));

	if (dev_core->product) {
		if (dev_core->manufacturer)
			strlcat(mouse->name, " ", sizeof(mouse->name));
		strlcat(mouse->name, dev_core->product, sizeof(mouse->name));
	}

	//Create the sysfs path aka virtual device path
	usb_make_path(dev_core, mouse->phys, sizeof(mouse->phys));
	strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

	//At this point your URB is configured, you can go ahead and fill it in
	usb_fill_int_urb(mouse->irq, dev_core, pipe, mouse->data,
			 (maxp > 8 ? 8 : maxp),
			 diprivi_mouse_irq, mouse, endpoint->bInterval);
			
	//You have to tell the URB to not create a DMA and that you have already
	//Create one. The next 2 lines do that
	mouse->irq->transfer_dma = mouse->data_dma;
	mouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	//Define and register the input device
	input_dev->name = mouse->name;
	input_dev->phys = mouse->phys;
	usb_to_input_id(dev_core, &input_dev->id);
	input_dev->dev.parent = &intf->dev;

	//Define the expected event type and subtype of keys
	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	//Let me just test left button alone
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT);

	//Pair the input_dev with the user defined struct
	input_set_drvdata(input_dev, mouse);

	//This is for polling. Instead of submitting URB and waiting for interrupts to occur
	//you submit the URB when the fOpen is called on the input device and you kill the URB
	//on call to fClose on input. This is basic Character device operations
	input_dev->open = diprivi_mouse_open;
	input_dev->close = diprivi_mouse_close;

	int error = input_register_device(mouse->indev);
	if (error){
		usb_free_urb(mouse->irq);
		usb_free_coherent(dev_core, 8, mouse->data, mouse->data_dma);
		input_free_device(input_dev);
		kfree(mouse);
		return -ENOMEM;
	}

	//Connect the interface to the custom struct
	//Why - Callback is based on the interface, and you need access the struct
	//from the call back. This is more like setIntentData and getIntentData
	usb_set_intfdata(intf, mouse);
	return 0;
}

static void diprivi_mouse_disconnect(struct usb_interface *intf)
{
	printk(KERN_WARNING "Diprivi mouse disconnected");
	struct diprivi_mouse *mouse = usb_get_intfdata (intf);

	//Deallocate all memory, and remove all references
	usb_set_intfdata(intf, NULL);
	if (mouse) {
		usb_kill_urb(mouse->irq);
		input_unregister_device(mouse->indev);
		usb_free_urb(mouse->irq);
		usb_free_coherent(interface_to_usbdev(intf), 8, mouse->data, mouse->data_dma);
		kfree(mouse);
	}
}

//List of target devices
static const struct usb_device_id diprivi_mouse_id_table[] = {
	{ USB_DEVICE(0x0458, 0x6001)},
	{ }
};

//Add the device to device table so that we can detect the 
//device the moment its plugged in
MODULE_DEVICE_TABLE (usb, diprivi_mouse_id_table);

//Describe the Driver to load
static struct usb_driver diprivi_mouse_driver = {
	.name		= "usbmouse",
	.probe		= diprivi_mouse_probe,
	.disconnect	= diprivi_mouse_disconnect,
	.id_table	= diprivi_mouse_id_table,
};

//Traditional Module Loading, this is just a USB wrapper
//over the traditional calls, venumna put traditional also
module_usb_driver(diprivi_mouse_driver);