# py -3.5 "$(SolutionDir)..\postbuild\copy_apps_to_vm.py" "C:\Program Files (x86)\VMware\VMware Virtual Disk Development Kit\bin\\" "C:\Program Files (x86)\VMware\VMware Workstation\\" "$(SolutionDir)..\VM\HAL9000_VM\\" "$(TargetDir).." "Z:"

import os
import subprocess
import shutil
import sys
import argparse
import string
import glob

class VmManager:
    def __init__(self, vm_tools_path, vmrun_path, vm_path):
        self.tools_bin_path = os.path.join(vm_tools_path, 'vmware-mount')
        self.vmrun_bin_path = os.path.join(vmrun_path, 'vmrun')
        self.vm_path = vm_path
        self.mounted = False


    def is_vm_running(self):
        completed = subprocess.run([self.vmrun_bin_path, 'list'], stdout=subprocess.PIPE)
        header, *vms = completed.stdout.decode('utf-8').splitlines()

        for vm in vms:
            if vm.startswith(self.vm_path):
                return True

        return False

    def get_latest_vmdk(self):
        vm_path = os.path.join(self.vm_path, 'HAL9000')
        pattern = vm_path + '*.vmdk'
        files = glob.glob(pattern)
        files = sorted(files)
        if len(files) == 0:
            return None
        # If there are shaphots the list will contain:
        # [HAL9000-00001.vmdk, HAL9000-00002.vmdk, HAL9000.vmdk]
        #  and we need the latest snaphot: HAL9000-00002.vmdk
        return files[0] if len(files) == 1 else files[-2]

    def mount(self, disk_letter):

        if os.path.exists(disk_letter):
            print('There already is a disk on {}!'.format(disk_letter))
            return False

        if self.is_vm_running():
            print('The disk is in use by a running VM!')
            return False

        if self.mounted == True:
            print('The disk is already mounted!')
            return False


        vmdk_path = self.get_latest_vmdk()

        if vmdk_path is None:
            print('Could not find the vmdk file!')
            return False

        completed = subprocess.run([self.tools_bin_path, disk_letter, vmdk_path])

        if completed.returncode != 0:
            print('Error mounting the disk: {}'.format(completed.stdout.strip()))
            return False

        self.mounted = True
        
        return True

    def unmount(self, disk_letter):
        if self.mounted == False:
            print('The disk is not mounted!')
            return False

        self.mounted = False

        subprocess.run([self.tools_bin_path, disk_letter, '/d'])
        return True


def main(args):

    vm_tools = VmManager(args.vm_tools_path, args.vmrun_path, args.vm_path)

    if args.dry_run:
        print("DRY RUN")

    vm_path = vm_tools.get_latest_vmdk()

    print('Mounting {} on {}'.format(vm_path, args.disk_letter))

    if args.dry_run == False:
        res = vm_tools.mount(args.disk_letter)

        if res == False:
            sys.exit(-1)

        assert(os.path.exists(args.disk_letter))

    for path in os.listdir(args.apps_path):
        _, ext = os.path.splitext(path)
        if ext == '.exe':
            src = os.path.join(args.apps_path, path)
            dst = os.path.join(args.disk_letter, 'Applications\\')
            print('Will copy {} to {}'.format(src, dst))

            if args.dry_run == False:
                shutil.copy(src, dst)

    print('Unmounting {}'.format(args.disk_letter))

    if args.dry_run == False:
        vm_tools.unmount(args.disk_letter)
    
    sys.exit(0)

def isdir(path):
    if not os.path.isdir(path):
        msg = '{} is not a valid directory'.format(path)
        raise argparse.ArgumentTypeError(msg)
    return os.path.normpath(path)

def isdisk_letter(arg):
    if (len(arg) != 2) or (arg[0] not in string.ascii_uppercase) or (arg[1] != ':'):
        msg = "{} does not respect the format '[A-Z]:'".format(arg)
        raise argparse.ArgumentTypeError(msg)
    return arg

if __name__ == '__main__':
    parser = argparse.ArgumentParser('copy_um_apps_to_vm')
    parser.add_argument('vm_tools_path', type=isdir)
    parser.add_argument('vmrun_path', type=isdir)
    parser.add_argument('vm_path', type=isdir)
    parser.add_argument('apps_path', type=isdir)
    parser.add_argument('disk_letter', type=isdisk_letter, nargs='?', default='Z:', help='Disk letter to mount to, Z: is the default')
    parser.add_argument('--dry-run', action='store_true', help='Only print actions that would be performed')

    args = parser.parse_args()

    main(args)