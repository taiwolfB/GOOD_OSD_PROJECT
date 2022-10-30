use strict;
use warnings;
use Archive::Zip qw(:ERROR_CODES);
use File::Spec::Functions;
use Cwd;

# 1st parameter: Path to paths.cmd file
# 2nd parameter: computer name
# 3rd parameter: a reference to a hash table with correspondance between key value to set
sub SetupPathsCmdFile
{
    local $/;

    my $cmdPath = $_[0];
    my $computerName = $_[1];
    my $hashValues = $_[2];
    my $newConfiguration = ":config\_$computerName\n\n";

    while (my ($key, $value) = each %$hashValues)
    {
        $newConfiguration .= "SET $key=\"$value\"\n";
    }
    
    $newConfiguration .= "goto end\n\n";
    
    open(my $fh, '<', $cmdPath) or die "Cannot open file for read at $cmdPath\n";
    
    my $data = <$fh>;
    close $fh;

    if ($data =~ /if _%COMPUTERNAME%_==\_$computerName\_ (goto config_)$computerName\n/i) {
        print "Updating existing configuration\n";
        $data =~ s/(:config_$computerName)(.*?)(goto end\n\n)/$newConfiguration/si;
    } else {
        print "Adding new configuration\n";
        $data =~ s/(if _%COMPUTERNAME%_==)(.*)(goto config_)(.*)/$1\_$computerName\_ $3$computerName\n$1$2$3$4/i;
    
        $data =~ s/(:end)/$newConfiguration$1/i;
    }

    open($fh, '>', $cmdPath) or die "Cannot open file for write at $cmdPath\n"; 
   
    print $fh $data;
    
    close $fh;

    print $data;
}

# a.k.a main

my $numArgs = $#ARGV + 1;

my $projectRootBasePath = File::Spec->rel2abs(catfile("..\\..", "HAL9000"));
my $vmPaths = catfile($projectRootBasePath, "VM");
my $pxePath = catfile($projectRootBasePath, "PXE");

print "Path to VM images is $vmPaths\n";

# Check for installed VMware
my $vmwarePath = catfile($ENV{'PROGRAMFILES(x86)'}, "VMware", "VMware Workstation");
if (!-d $vmwarePath) {
    print sprintf("Could not find VMware in it's default location! %s\n", $vmwarePath);
    if ($numArgs != 1) {
        die "VMware not found, please specify install location of VMware as a command line argument to continue HAL setup\n";
    }
    $vmwarePath = $ARGV[0];
    if (!-d $vmwarePath) {
        die sprintf("Specified path: %s is not an existing directory\n", $vmwarePath);
    }
}

print sprintf("VMware path is: %s\n", $vmwarePath);

# We need to parse the paths.cmd file and complete with this computer's paths
print sprintf("Computer name is %s\n", $ENV{'COMPUTERNAME'});

my %hOptions =
(
    'VOL_MOUNT_LETTER' => "Q:",
    'PXE_PATH' => "$pxePath",
    'PATH_TO_VM_DISK' => catfile($vmPaths, "HAL9000_VM", "HAL9000.vmdk"),
    'PATH_TO_VIX_TOOLS' => $vmwarePath,
    'PATH_TO_LOG_FILE' => catfile($vmPaths, "HAL9000_VM", "HAL9000.log"),
    'PATH_TO_VM_FILE' => catfile($vmPaths, "HAL9000_VM", "HAL9000.vmx"),
);

my $vmtoolsPath = catfile($ENV{'PROGRAMFILES(x86)'}, "VMware", "VMware Virtual Disk Development Kit");
if (!-d $vmtoolsPath) {
    print sprintf("Could not find VMware Virtual Disk Development Kit in it's default location! %s\n", $vmtoolsPath);
    if ($numArgs == 2) {
        $vmtoolsPath = $ARGV[1];
        if (!-d $vmtoolsPath) {
            die sprintf("Specified path: %s is not an existing directory\n", $vmtoolsPath);
        }
        $hOptions{'PATH_TO_VM_TOOLS'} = $vmtoolsPath;
        print sprintf("VMware Virtual Disk Development Kit path is: %s\n", $vmtoolsPath);
    }
} else {
    $hOptions{'PATH_TO_VM_TOOLS'} = $vmtoolsPath;
    print sprintf("VMware Virtual Disk Development Kit path is: %s\n", $vmtoolsPath);
}

SetupPathsCmdFile( catfile($projectRootBasePath, "postbuild", "paths.cmd"), $ENV{'COMPUTERNAME'}, \%hOptions);