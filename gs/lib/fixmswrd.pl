#!/usr/bin/perl
# 

# 

#   (C) 1997 Anthony Shipman
# 
#   This software is provided 'as-is', without any express or implied
#   warranty.  In no event will the authors be held liable for any damages
#   arising from the use of this software.
# 
#   Permission is granted to anyone to use this software for any purpose,
#   including commercial applications, and to alter it and redistribute it
#   freely, subject to the following restrictions:
# 
#   1. The origin of this software must not be misrepresented; you must not
#      claim that you wrote the original software. If you use this software
#      in a product, an acknowledgment in the product documentation would be
#      appreciated but is not required.
#   2. Altered source versions must be plainly marked as such, and must not be
#      misrepresented as being the original software.
#   3. This notice may not be removed or altered from any source distribution.
# 
#   Anthony Shipman    shipmana@acm.org

# This program patches the postscript generated by MS Word printer drivers
# so that they work with ghostview 1.5.  The problem is that the document
# structuring conventions are not followed by Word.  The pages are supposed
# to be independent but they depend on a dictionary being opened outside
# of the pages.  The erroneous structure is
# 
# 	%%EndSetup
# 	NTPSOct95 begin
# 	%%Page: 1 1
# 	<text>
# 	showpage
# 	%%Page: 2 2
# 	<text>
# 	showpage
# 	......
# 	%%Trailer
# 	...
# 	end
# 	%%EOF
# 
# This only works if the all of the structure around the pages is preserved.
# The opening of NTPSOct95 happens outside of any structured section so
# it is never seen by ghostview.  We change the structure to
# 
# 	%%EndSetup
# 	%%Page: 1 1
# 	NTPSOct95 begin
# 	<text>
# 	showpage
# 	end
# 	%%Page: 2 2
# 	NTPSOct95 begin
# 	<text>
# 	showpage
# 	end
# 	......
# 	%%Trailer
# 	...
# 	%%EOF
# 
# That is the dictionary opening is repeated inside each page.
# 
# We add a comment to the document to mark that it has been converted.
# This has the form 
#	%LOCALGhostviewPatched
#
# Usage:
#	fixmswrd [-v] [file [output-file]]

require 'getopts.pl';

#=================================================================

$program = "fixmswrd";

sub usage {
    die "Usage: $program [-v] [file [output-file]]\n";
}

#=================================================================

&Getopts("v") || &usage;

$verbose = $opt_v;


$infile = shift(@ARGV);
if ($infile)
{
    open(INFILE, $infile) || die "$program: Cannot read from $infile\n";
    $handle = "INFILE";
}
else
{
    $handle = "STDIN";
}


$outfile = shift(@ARGV);
if ($outfile)
{
    open(OUTFILE, ">$outfile") || die "$program: Cannot write to $outfile\n";
    select(OUTFILE);
}

#  This reads the header comments and detects the presence of the marker.
$have_marker = 0;

undef $dict_name;
undef $dict_line;

&read_comments;
&put_comments;

if ($have_marker)
{
    $verbose && print STDERR "$program: Warning - already converted\n";

    while(<$handle>)		# pass the file through unchanged.
    {
	print;
    }
}
else
{
    $seen_trailer = 0;

    while(<$handle>)		# massage the file
    {
	if ($dict_line)
	{
	    next if (/$dict_line/o);		# drop the old begin line
	    $seen_trailer = 1 if (/^%%Trailer/);
	    next if ($seen_trailer and /^end/);	# drop the old end line
	}

	print;

	if (/^%%Page:/)
	{
	    print "$dict_name begin\n";	# add at the start of the page
	}
	elsif (/^showpage/)
	{
	    print "end\n";			# add at the end of the page
	}
	elsif (/^%%BeginResource: procset (\S+)/)
	{
	    $dict_name = $1;
	    $dict_line = "^$dict_name begin";
	}
	elsif (/^%%BeginProcSet: (\S+)/)	# for older document versions
	{
	    $dict_name = $1;
	    $dict_line = "^$dict_name begin";
	}
	elsif (/^%%EndProlog:/)
	{
	    unless ($dict_line) 
	    {
		$verbose && 
		    print STDERR "$program: Warning - unrecognised document structure\n";
	    }
	}
    }
}

exit 0;

#=================================================================


#  This reads all of the header comments into an array which we can write
#  out again later.  In addition we detect the presence of the marker comment.

sub read_comments
{
    @headers = ();

    while (<$handle>)
    {				# without chopping
	push(@headers, $_);
	if (/^%LOCALGhostviewPatched/)
	{
	    $have_marker = 1;
	}
	last if /^%%EndComments/;
    }
}



sub put_comments 
{
    foreach $h (@headers)
    {
	if (!$have_marker and ($h =~ /^%%EndComments/))
	{
	    print "%LOCALGhostviewPatched\n";
	}
	print $h;		# contains the newline
    }
}
