"""This file contains the DeviceManager class."""
 
import sys
import gobject
import gtk
import dbus

try:
    import gnome.ui
except ImportError:
    gnome_imported = 0
else:
    gnome_imported = 1

import Const
from Representation import Representation
from Device import Device

from LibGladeApplication import LibGladeApplication
 
class DeviceManager(LibGladeApplication):
    """This is the main window for the application."""


    def on_about_activate(self, w):
        """Show the about dialog."""
        gnome.ui.About(Const.NAME_LONG, Const.VERSION, Const.COPYRIGHT,
                       Const.INFO, Const.AUTHORS).show()

    def on_virtual_devices_activate(self, obj):
        self.dont_show_virtual = 1 - self.dont_show_virtual
        self.update_device_list()

    def __init__(self):
        """Init the GUI and connect to the HAL daemon."""
        LibGladeApplication.__init__(self, Const.DATADIR + "/hal-device-manager.glade")

        if not gnome_imported:
            self.xml.get_widget("about1").set_sensitive(0)

        self.representation = Representation()

        self.bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)
        self.hal_service = self.bus.get_service("org.freedesktop.Hal")
        self.hal_manager = self.hal_service.get_object("/org/freedesktop/Hal/Manager",
                                         "org.freedesktop.Hal.Manager")

        # gdl_changed will be invoked when the Global Device List is changed
        # per the hal spec
        self.bus.add_signal_receiver(self.gdl_changed,
				     "DeviceAdded",
                                     "org.freedesktop.Hal.Manager",
                                     "org.freedesktop.Hal",
                                     "/org/freedesktop/Hal/Manager")
        self.bus.add_signal_receiver(self.gdl_changed,
				     "DeviceRemoved",
                                     "org.freedesktop.Hal.Manager",
                                     "org.freedesktop.Hal",
                                     "/org/freedesktop/Hal/Manager")
        self.bus.add_signal_receiver(self.gdl_changed,
				     "NewCapability",
                                     "org.freedesktop.Hal.Manager",
                                     "org.freedesktop.Hal",
                                     "/org/freedesktop/Hal/Manager")

        # Add listeners for all devices
        try:
            device_names = self.hal_manager.GetAllDevices()
        except:
            dialog = gtk.MessageDialog(None, gtk.DIALOG_MODAL, 
                                       gtk.MESSAGE_ERROR, gtk.BUTTONS_CLOSE,
                                       "Could not get device list. "
                                       "Make sure hald is running!")
            dialog.run()
            sys.exit(1)

        for name in device_names:
	    self.add_device_signal_recv (name);

        self.dont_show_virtual = 1
        self.update_device_list()
        self.main_window.show()

    def add_device_signal_recv (self, udi):
	self.bus.add_signal_receiver(self.device_changed,
				     "PropertyModified",
				     "org.freedesktop.Hal.Device",
				     "org.freedesktop.Hal",
				     udi)
	return
	self.bus.add_signal_receiver(self.device_changed,
				     "Condition",
				     "org.freedesktop.Hal.Device",
				     "org.freedesktop.Hal",
				     udi)

    def remove_device_signal_recv (self, udi):
	self.bus.remove_signal_receiver(self.device_changed,
				     "PropertyModified",
				     "org.freedesktop.Hal.Device",
				     "org.freedesktop.Hal",
				     udi)
	return
	self.bus.remove_signal_receiver(self.device_changed,
				     "Condition",
				     "org.freedesktop.Hal.Device",
				     "org.freedesktop.Hal",
				     udi)


    def get_current_focus_udi(self):
        """Get the UDI of the currently focused device"""
        (tree_model, tree_iter) = self.tree_selection.get_selected()
        if tree_iter:
            device_udi = tree_model.get_value(tree_iter, Const.UDI_COLUMN)
            return device_udi
        return None

    def on_device_tree_selection_changed(self, tree_selection):
        """This method is called when the selection has changed in the
        device tree"""
        device_udi = self.get_current_focus_udi()
        if device_udi != None:
            device = self.udi_to_device(device_udi)
            self.update_device_notebook(device)


    def device_changed(self, dbus_if, dbus_member, dbus_svc,
                       dbus_obj_path, dbus_message):
        """This method is called when signals on the Device interface is
        received"""
        if dbus_member=="Condition":
            args = dbus_message.get_args_list()
            print "\nCondition %s, device=%s"%(args[0], dbus_obj_path)
            print "  message = ", args 
        elif dbus_member=="PropertyModified":
            args = dbus_message.get_args_list()
            num_modifications = args[0]
            print "\nPropertyModified, device=%s"%dbus_obj_path
            #print "dbus_obj_path", dbus_obj_path
            for i in range(0, num_modifications):
                property_name = args[1+3*i]
                removed = args[2+3*i]
                added = args[3+3*i]

                print "  key=%s, rem=%d, add=%d"%(property_name, removed, added)
                if property_name=="info.parent":
                    self.update_device_list()        
                else:
                    device_udi = dbus_obj_path
                    device_udi_obj = self.hal_service.get_object(device_udi,
                                                   "org.freedesktop.Hal.Device")
                    device_obj = self.udi_to_device(device_udi)

                    if device_udi_obj.PropertyExists(property_name):
                        device_obj.properties[property_name] = device_udi_obj.GetProperty(property_name)
                        print "  value=%s"%(device_obj.properties[property_name])
                    else:
                        if device_obj != None:
                            try:
                                del device_obj.properties[property_name]
                            except:
                                pass

                    device_focus_udi = self.get_current_focus_udi()
                    if device_focus_udi != None:
                        device = self.udi_to_device(device_udi)
                        if device_focus_udi==device_udi:
                            self.update_device_notebook(device)


    def gdl_changed(self, dbus_if, dbus_member, dbus_svc, dbus_obj_path, dbus_message):
        """This method is called when a HAL device is added or removed."""

        if dbus_member=="DeviceAdded":
            [device_udi] = dbus_message.get_args_list()
            print "\nDeviceAdded, udi=%s"%(device_udi)
	    self.add_device_signal_recv (device_udi)
            self.update_device_list()
        elif dbus_member=="DeviceRemoved":
            [device_udi] = dbus_message.get_args_list()
            print "\nDeviceRemoved, udi=%s"%(device_udi)
	    self.remove_device_signal_recv (device_udi)
            self.update_device_list()
        elif dbus_member=="NewCapability":
            [device_udi, cap] = dbus_message.get_args_list()
            print "\nNewCapability, cap=%s, udi=%s"%(cap, device_udi)
        else:
            print "*** Unknown signal %s"%dbus_member


    def update_device_list(self):
        """Builds, or rebuilds, the device tree"""
        # We use a virtual root device so we have a single tree
        self.virtual_root = self.build_device_tree()

        # (Name to display, device UDI)
        try:
            if self.tree_model:
                pass
        except:
            self.tree_model = gtk.TreeStore(gtk.gdk.Pixbuf,
                                            gobject.TYPE_STRING, gobject.TYPE_STRING)
        while 1:
            it = self.tree_model.get_iter_first()
            if not it:
                break
            self.tree_model.remove(it)

        self.virtual_root.populate_gtk_tree(self.tree_model,
                                            self.dont_show_virtual,
                                            self.representation)

        tree_view = self.xml.get_widget("device_tree")
        try:
            if self.tree_selection:
                pass
        except:
            self.tree_selection = tree_view.get_selection()
            self.tree_selection.connect("changed",
                                        self.on_device_tree_selection_changed)

        # add new columns only first time
        try:
            if self.column_dt:
                pass
        except:
            self.column_dt = gtk.TreeViewColumn()
            self.column_dt.set_title("Devices")
            render_pixbuf = gtk.CellRendererPixbuf()
            self.column_dt.pack_start(render_pixbuf, expand=False)
            self.column_dt.add_attribute(render_pixbuf, "pixbuf",
                                         Const.PIXBUF_COLUMN)
            render_text = gtk.CellRendererText()
            self.column_dt.pack_start(render_text, expand=True)
            self.column_dt.add_attribute(render_text, "text",
                                         Const.TITLE_COLUMN)
            tree_view.append_column(self.column_dt)

        tree_view.set_model(self.tree_model)
        tree_view.expand_all()

        # Set focus to first element
        tree_view.grab_focus()
        self.update_device_notebook(self.virtual_root.children[0])


    def udi_to_device(self, device_udi):
        """Given a HAL UDI (Unique Device Identifier) this method returns
        the corresponding HAL device"""
        return self.virtual_root.find_by_udi(device_udi)

    def build_device_tree(self):
        """Retrieves the device list from the HAL daemon and builds
        a tree of Device (Python) objects. The root is a virtual
        device"""
        device_names = self.hal_manager.GetAllDevices()
        device_names.sort()

        virtual_root = Device("virtual_root", None, {})
        self.device_list = [virtual_root]
        
        # first build list of Device objects
        for name in device_names:
            device_dbus_obj = self.hal_service.get_object(name,
                                                     "org.freedesktop.Hal.Device")
            properties = device_dbus_obj.GetAllProperties()
            try:
                parent_name = properties["info.parent"]
            except KeyError:
                # no parent, must be parent of virtual_root
                parent_name = "/"
            device = Device(name, parent_name, properties)
            self.device_list.append(device)

        # set parent_device and children for each Device object
        for device in self.device_list:
            parent_name = device.parent_name
            device.parent_device = virtual_root
            if parent_name!="/":
                for p in self.device_list:
                    if p.device_name==parent_name:
                        device.parent_device = p
                        p.children.append(device)
            if device!=virtual_root and device.parent_device==virtual_root:
                virtual_root.children.append(device)
            if device==virtual_root:
                device.parent_device=None
        return virtual_root


    def update_tab_device(self, device):
        """Updates the 'Device' tab given a Device object"""
        bus = self.xml.get_widget("ns_device_bus")
        #state = self.xml.get_widget("ns_device_status")
        vendor = self.xml.get_widget("ns_device_vendor")
        product = self.xml.get_widget("ns_device_name")
        category = self.xml.get_widget("ns_device_category")
        capabilities = self.xml.get_widget("ns_device_capabilities")

        # we always have Bus and State
        bus.set_label(Const.BUS_NAMES[device.properties["info.bus"]])
        #state.set_label(Const.STATE_NAMES[device.properties["State"]])

        # guestimate product and vendor if we have no device information file
        if device.properties["info.bus"]=="usb":
            if device.properties.has_key("info.product"):
                product.set_label("%s"%device.properties["info.product"])
            elif device.properties.has_key("usb.product"):
                product.set_label("%s"%device.properties["usb.product"])
            elif device.properties.has_key("usb.product_id"):
                product.set_label("Unknown (0x%x)"%device.properties["usb.product_id"])
            else:
                product.set_label("Unknown")

            if device.properties.has_key("info.vendor"):
                vendor.set_label("%s"%device.properties["info.vendor"])
            elif device.properties.has_key("usb.vendor"):
                vendor.set_label("%s"%device.properties["usb.vendor"])
            elif device.properties.has_key("usb.vendor_id"):
                vendor.set_label("Unknown (0x%x)"%device.properties["usb.vendor_id"])
            else:
                vendor.set_label("Unknown")


        elif device.properties["info.bus"]=="pci":
            if device.properties.has_key("info.product"):
                product.set_label("%s"%device.properties["info.product"])
            elif device.properties.has_key("pci.product"):
                product.set_label("%s"%device.properties["pci.product"])
            elif device.properties.has_key("pci.product_id"):
                product.set_label("Unknown (0x%x)"%device.properties["pci.product_id"])
            else:
                product.set_label("Unknown")

            if device.properties.has_key("info.vendor"):
                vendor.set_label("%s"%device.properties["info.vendor"])
            elif device.properties.has_key("pci.vendor"):
                vendor.set_label("%s"%device.properties["pci.vendor"])
            elif device.properties.has_key("pci.vendor_id"):
                vendor.set_label("Unknown (0x%x)"%device.properties["pci.vendor_id"])
            else:
                vendor.set_label("Unknown")
        elif device.properties["info.bus"]=="block":
            if device.properties.has_key("info.product"):
                product.set_label("%s"%device.properties["info.product"])
            else:
                product.set_label("Unknown")

            if device.properties.has_key("info.vendor"):
                vendor.set_label("%s"%device.properties["info.vendor"])
            else:
                vendor.set_label("Unknown")
        else:
            product.set_label("Unknown")
            vendor.set_label("Unknown")

        # clear category, capabilities
        # set category, capabilities
        if device.properties.has_key("info.category"):
            category.set_label("%s"%device.properties["info.category"])
        else:
	    category.set_label("Unknown")

	if device.properties.has_key("info.capabilities"):
            capabilities.set_label("%s"%device.properties["info.capabilities"])
	else:
	    capabilities.set_label("Unknown")

    def update_tab_usb(self, device):
        """Updates the 'USB' tab given a Device object; may hide it"""
        page = self.xml.get_widget("device_notebook").get_nth_page(1)
        if device.properties["info.bus"]!="usb":
            page.hide_all()
            return

        page.show_all()

        version = self.xml.get_widget("ns_usb_version")
        bandwidth = self.xml.get_widget("ns_usb_bandwidth")
        maxpower = self.xml.get_widget("ns_usb_maxpower")
        man_id = self.xml.get_widget("ns_usb_man_id")
        prod_id = self.xml.get_widget("ns_usb_prod_id")
        revision = self.xml.get_widget("ns_usb_rev")

        bcdVersion = device.properties["usb.version_bcd"]
        version.set_label("%x.%x"%(bcdVersion>>8, bcdVersion&0xff))

        bcdSpeed = device.properties["usb.speed_bcd"]
        bandwidth.set_label("%x.%x Mbit/s"%(bcdSpeed>>8, bcdSpeed&0xff))
        maxpower.set_label("%d mA"%(device.properties["usb.max_power"]))
        if not device.properties.has_key("usb.vendor"):
            man_id.set_label("0x%04x"%(device.properties["usb.vendor_id"]))
        else:
            man_id.set_label("%s"%(device.properties["usb.vendor"]))
        if not device.properties.has_key("usb.product"):
            prod_id.set_label("0x%04x"%(device.properties["usb.product_id"]))
        else:
            prod_id.set_label("%s"%(device.properties["usb.product"]))
        bcdDevice = device.properties["usb.device_revision_bcd"]
        revision.set_label("%x.%x"%((bcdDevice>>8), bcdDevice&0xff))


    def update_tab_pci(self, device):
        """Updates the 'PCI' tab given a Device object; may hide it"""
        page = self.xml.get_widget("device_notebook").get_nth_page(2)
        if device.properties["info.bus"]!="pci":
            page.hide_all()
            return

        page.show_all()

        man_id = self.xml.get_widget("ns_pci_man_id")
        prod_id = self.xml.get_widget("ns_pci_prod_id")
        subsys_man_id = self.xml.get_widget("ns_pci_subsys_man_id")
        subsys_prod_id = self.xml.get_widget("ns_pci_subsys_prod_id")

        if not device.properties.has_key("pci.vendor"):
            man_id.set_label("Unknown (0x%04x)"%(device.properties["pci.vendor_id"]))
        else:
            man_id.set_label("%s"%(device.properties["pci.vendor"]))
        if not device.properties.has_key("pci.product"):
            prod_id.set_label("Unknown (0x%04x)"%(device.properties["pci.product_id"]))
        else:
            prod_id.set_label("%s"%(device.properties["pci.product"]))

        if not device.properties.has_key("pci.subsys_vendor"):
            subsys_man_id.set_label("Unknown (0x%04x)"%(device.properties["pci.subsys_vendor_id"]))
        else:
            subsys_man_id.set_label("%s"%(device.properties["pci.subsys_vendor"]))
        if not device.properties.has_key("pci.subsys_product"):
            subsys_prod_id.set_label("Unknown (0x%04x)"%(device.properties["pci.subsys_product_id"]))
        else:
            subsys_prod_id.set_label("%s"%(device.properties["pci.subsys_product"]))

    def update_tab_advanced(self, device):
        """Updates the 'Advanced' tab given a Device object"""
        store = gtk.ListStore(gobject.TYPE_STRING,
                              gobject.TYPE_STRING,
                              gobject.TYPE_STRING)
        keys = device.properties.keys()
        keys.sort()
        for p in keys:
            iter = store.append()
            val = device.properties[p]
            ptype = type(val)
            if ptype==str:
                store.set(iter, 0, p, 1, "string", 2, "%s"%val)
            elif ptype==int:
                store.set(iter, 0, p, 1, "int", 2, "%d (0x%x)"%(val, val))
            elif ptype==long:
                store.set(iter, 0, p, 1, "long", 2, "%d (0x%x)"%(val, val))
            elif ptype==bool:
                if val:
                    store.set(iter, 0, p, 1, "bool", 2, "true")
                else:
                    store.set(iter, 0, p, 1, "bool", 2, "false")
            elif ptype==float:
                store.set(iter, 0, p, 1, "float", 2, "%f"%val)


        prop_tree_view = self.xml.get_widget("ns_adv_properties")

        # remove old columns, if any
        cols = prop_tree_view.get_columns()
        for cr in cols:
            prop_tree_view.remove_column(cr)

        column0 = gtk.TreeViewColumn("Key", gtk.CellRendererText(), text=0)
        column1 = gtk.TreeViewColumn("Type", gtk.CellRendererText(), text=1)
        column2 = gtk.TreeViewColumn("Value", gtk.CellRendererText(), text=2)
        prop_tree_view.append_column(column0)
        prop_tree_view.append_column(column1)
        prop_tree_view.append_column(column2)

        prop_tree_view.set_model(store)


    def update_device_notebook(self, device):
        """Updates the entire notebook of tabs given a Device object"""
        self.update_tab_device(device)
        self.update_tab_advanced(device)
        self.update_tab_usb(device)
        self.update_tab_pci(device)
