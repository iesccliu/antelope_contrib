#
# make_dbrecenteqs_map
# script to make an Antelope dbrecenteqs or dbevents-style map
# Kent Lindquist
# Lindquist Consulting
# 2004
#

require "getopts.pl" ;
require "dbrecenteqs.pl";
require "dbgmtgrid.pl";
require "winding.pl";
require "compass_from_azimuth.pl";
use Datascope;
use Image::Magick;

elog_init( $0, @ARGV );

if ( ! &Getopts('vp:') || @ARGV != 1 ) {

	die ( "Usage: $Program [-v] [-p pffile] psfile\n" );

} else {

	$psfile = $ARGV[0];

	if( $psfile !~ /\.ps$/ ) {

		$psfile .= ".ps";
	}

	if( $opt_p ) {
		$State{pf} = $opt_p;
	} else {
		$State{pf} = "make_dbrecenteqs_map";
	}

	if( $opt_v ) {
		$V = "-V";
	} else {
		$V = "";
	}
}

setup_State();

$hashref = pfget( $State{pf}, "mapspec" );
%Mapspec = %{$hashref};

%Mapspec = %{setup_index_Mapspec( \%Mapspec )};

$Mapspec{psfile} = "$psfile";
$Mapspec{pixfile} = "$psfile";
$Mapspec{pixfile} =~ s/.ps$/.$Mapspec{format}/;

$Mapspec{mapname} = (parsepath($Mapspec{psfile}))[1];
$Mapspec{source} = "dynamic";
$Mapspec{contour_mode} = "grddb";
$Mapspec{mapclass} = "index";
$Mapspec{longitude_branchcut_high} = 360;

plot_basemap( \%Mapspec, "first" );
plot_contours( \%Mapspec, "middle" );
plot_coastlines( \%Mapspec, "middle" );
plot_lakes( \%Mapspec, "middle" );
plot_rivers( \%Mapspec, "middle" );
plot_national_boundaries( \%Mapspec, "middle" );
plot_state_boundaries( \%Mapspec, "middle" );
plot_linefiles( \%Mapspec, "middle" );
plot_basemap( \%Mapspec, "middle" );
plot_cities( \%Mapspec, "last" );

%Mapspec = %{pixfile_convert( \%Mapspec )};
write_pixfile_pffile( \%Mapspec );