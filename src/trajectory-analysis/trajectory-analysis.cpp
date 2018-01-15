#include <algorithm>	// for std::max_element()
#include <cmath>		// for std::sqrt()
#include <fstream>
#include <iomanip>
#include <string>
#include <cstring>
#include <ctime>
#include <regex>

#include <gromacs/topology/atomprop.h>
#include <gromacs/random/threefry.h>
#include <gromacs/random/uniformrealdistribution.h>
#include <gromacs/fileio/confio.h>
#include <gromacs/utility/programcontext.h>

#include "trajectory-analysis/trajectory-analysis.hpp"

#include "aggregation/boltzmann_energy_calculator.hpp"
#include "aggregation/number_density_calculator.hpp"
#include "aggregation/multiscalar_time_series.hpp"
#include "aggregation/scalar_time_series.hpp"

#include "config/config.hpp"
#include "config/version.hpp"

#include "geometry/cubic_spline_interp_1D.hpp"
#include "geometry/cubic_spline_interp_3D.hpp"
#include "geometry/linear_spline_interp_1D.hpp"
#include "geometry/spline_curve_1D.hpp"
#include "geometry/spline_curve_3D.hpp"

#include "io/analysis_data_json_frame_exporter.hpp"
#include "io/json_doc_importer.hpp"
#include "io/molecular_path_obj_exporter.hpp"
#include "io/multiscalar_time_series_json_converter.hpp"
#include "io/results_json_exporter.hpp"
#include "io/spline_curve_1D_json_converter.hpp"
#include "io/summary_statistics_json_converter.hpp"
#include "io/summary_statistics_vector_json_converter.hpp"

#include "statistics/amise_optimal_bandwidth_estimator.hpp"
#include "statistics/histogram_density_estimator.hpp"
#include "statistics/kernel_density_estimator.hpp"
#include "statistics/summary_statistics.hpp"
#include "statistics/weighted_kernel_density_estimator.hpp"

#include "trajectory-analysis/analysis_data_long_format_plot_module.hpp"
#include "trajectory-analysis/analysis_data_pdb_plot_module.hpp"

#include "path-finding/inplane_optimised_probe_path_finder.hpp"
#include "path-finding/optimised_direction_probe_path_finder.hpp"
#include "path-finding/naive_cylindrical_path_finder.hpp"
#include "path-finding/vdw_radius_provider.hpp"

using namespace gmx;



/*
 * Constructor for the trajectoryAnalysis class.
 */
trajectoryAnalysis::trajectoryAnalysis()
    : pfProbeRadius_(0.0)
    , pfMaxProbeSteps_(1e3)
    , pfInitProbePos_(3)
    , pfChanDirVec_(3)
    , saMaxCoolingIter_(1e3)
    , saNumCostSamples_(50)
    , saConvRelTol_(1e-10)
    , saInitTemp_(10.0)
    , saCoolingFactor_(0.99)
    , saStepLengthFactor_(0.01)
    , saUseAdaptiveCandGen_(false)
{
    registerAnalysisDataset(&frameStreamData_, "frameStreamData");
    frameStreamData_.setMultipoint(true); 

    // register internal timing dataset:
    registerAnalysisDataset(&timingData_, "timingData");

    // default initial probe position and chanell direction:
    pfInitProbePos_ = {0.0, 0.0, 0.0};
    pfChanDirVec_ = {0.0, 0.0, 1.0};


}



/*
 *
 */
void
trajectoryAnalysis::initOptions(IOptionsContainer          *options,
                                TrajectoryAnalysisSettings *settings)
{    
    // HELP TEXT
    //-------------------------------------------------------------------------

	// set help text:
	static const char *const desc[] = {
		"This is a first prototype for the CHAP tool.",
		"There is NO HELP, you are on your own!"
	};
    settings -> setHelpText(desc);


    // SETTINGS
    //-------------------------------------------------------------------------

	// require the user to provide a topology file input:
    settings -> setFlag(TrajectoryAnalysisSettings::efRequireTop);

    // will not use periodic boundary conditions:
    // TODO set PBC back to true
    settings -> setPBC(false);
    settings -> setFlag(TrajectoryAnalysisSettings::efNoUserPBC);

    // will make molecules whole:
    settings -> setRmPBC(false);
    settings -> setFlag(TrajectoryAnalysisSettings::efNoUserRmPBC);

    // will use coordinates from topology:
    settings -> setFlag(TrajectoryAnalysisSettings::efUseTopX, true);


    // SELECTION OPTIONS
    //-------------------------------------------------------------------------

	options -> addOption(SelectionOption("sel-pathway")
	                     .store(&refsel_).required()
		                 .description("Reference group that defines the "
                                      "permeation pathway (usually "
                                      "'Protein') "));

	options -> addOption(SelectionOption("sel-solvent")
                         .storeVector(&sel_)
	                     .description("Group of small particles to calculate "
                                      "density of (usually 'Water')"));


    // OUTPUT OPTIONS
    // ------------------------------------------------------------------------

    // TODO:not currently used, but potentially useful in future
    /*
    options -> addOption(IntegerOption("num-out-pts")
                         .store(&nOutPoints_)
                         .defaultValue(1000)
                         .description("Number of sample points of pore centre "
                                      "line that are written to output."));*/

    options -> addOption(StringOption("out-filename")
	                     .store(&outputBaseFileName_)
                         .defaultValue("output")
                         .description("File name for output files without "
                                      "file extension. Proper file extensions "
                                      "(e.g. filename.json) will be added "
                                      "internally."));

    options -> addOption(IntegerOption("out-num-points")
	                     .store(&outputNumPoints_)
                         .defaultValue(1000)
                         .description("."));

    options -> addOption(RealOption("out-extrap-dist")
	                     .store(&outputExtrapDist_)
                         .defaultValue(0.0)
                         .description("."));


    // PATH FINDING PARAMETERS
    //-------------------------------------------------------------------------

    const char * const allowedPathFindingMethod[] = {"naive_cylindrical",
                                                     "inplane_optim"};
    pfMethod_ = ePathFindingMethodInplaneOptimised;                                         
    options -> addOption(EnumOption<ePathFindingMethod>("pf-method")
                         .enumValue(allowedPathFindingMethod)
                         .store(&pfMethod_)
                         .description("Path finding method. The default "
                                      "inplane_optim implements the algorithm "
                                      "used in the HOLE programme, where the "
                                      "position of a probe sphere is "
                                      "optimised in subsequent parallel "
                                      "planes so as to maximise its radius. "
                                      "The alternative naive_cylindrical "
                                      "simply uses a cylindrical volume as "
                                      "permeation pathway."));

    const char * const allowedVdwRadiusDatabase[] = {"hole_amberuni",
                                                     "hole_bondi",
                                                     "hole_hardcore",
                                                     "hole_simple", 
                                                     "hole_xplor",
                                                     "user"};
    pfVdwRadiusDatabase_ = eVdwRadiusDatabaseHoleSimple;
    options -> addOption(EnumOption<eVdwRadiusDatabase>("pf-vdwr-database")
                         .enumValue(allowedVdwRadiusDatabase)
                         .store(&pfVdwRadiusDatabase_)
                         .description("Database of van-der-Waals radii to be "
                                      "used in pore finding"));

    options -> addOption(RealOption("pf-vdwr-fallback")
                         .store(&pfDefaultVdwRadius_)
                         .storeIsSet(&pfDefaultVdwRadiusIsSet_)
                         .defaultValue(-1.0)
                         .description("Fallback van-der-Waals radius for "
                                      "atoms that are not listed in "
                                      "van-der-Waals radius database. If "
                                      "negative, an error will be thrown if "
                                      "the database does not contain a "
                                      "van-der-Waals radii for all particles "
                                      "in the pathway defining group."));

    options -> addOption(StringOption("pf-vdwr-json")
                         .store(&pfVdwRadiusJson_)
                         .storeIsSet(&pfVdwRadiusJsonIsSet_)
                         .description("JSON file with user defined "
                                      "van-der-Waals radii. Will be "
                                      "ignored unless -pf-vdwr-database is "
                                      "set to 'user'."));

    const char * const allowedPathAlignmentMethod[] = {"none",
                                                       "ipp"};
    pfPathAlignmentMethod_ = ePathAlignmentMethodIpp;
    options -> addOption(EnumOption<ePathAlignmentMethod>("pf-align-method")
                         .enumValue(allowedPathAlignmentMethod)
                         .store(&pfPathAlignmentMethod_)
                         .description("Method for aligning pathway "
                                      "coordinates across time steps"));

    options -> addOption(RealOption("pf-probe-step")
                         .store(&pfProbeStepLength_)
                         .defaultValue(0.025)
                         .description("Step length for probe movement."));

    options -> addOption(RealOption("pf-max-free-dist")
                         .store(&pfMaxProbeRadius_)
                         .defaultValue(1.0)
                         .description("Maximum radius of pore."));

    options -> addOption(IntegerOption("pf-max-probe-steps")
                         .store(&pfMaxProbeSteps_)
                         .defaultValue(10000)
                         .description("Maximum number of steps the probe is "
                                      "moved in either direction."));

    options -> addOption(SelectionOption("pf-sel-ipp")
                         .store(&ippsel_)
                         .storeIsSet(&ippselIsSet_)
	                     .description("Reference group from which to "
                                      "determine the initial probe position "
                                      "for the path finding algorithm. If "
                                      "unspecified, this defaults to the "
                                      "overall path defining group. Will be "
                                      "overridden if init-probe-pos is set "
                                      "explicitly."));

    options -> addOption(RealOption("pf-init-probe-pos")
                         .storeVector(&pfInitProbePos_)
                         .storeIsSet(&pfInitProbePosIsSet_)
                         .valueCount(3)
                         .description("Initial position of probe in "
                                      "probe-based pore finding algorithms. "
                                      "If set explicitly, it will overwrite "
                                      "the COM-based initial position set "
                                      "with the ippselflag."));

    std::vector<real> chanDirVec_ = {0.0, 0.0, 1.0};
    options -> addOption(RealOption("pf-chan-dir-vec")
                         .storeVector(&pfChanDirVec_)
                         .storeIsSet(&pfChanDirVecIsSet_)
                         .valueCount(3)
                         .description("Channel direction vector. Will be "
                                      "normalised to unit vector internally. "
                                      "If unset pore is assumed to be "
                                      "oriented in z-direction."));
   
    // max-free-dist and largest vdW radius
    options -> addOption(DoubleOption("pf-cutoff")
	                     .store(&cutoff_)
                         .storeIsSet(&cutoffIsSet_)
                         .description("Cutoff for distance searches in path "
                                      "finding algorithm. A value of zero "
                                      "or less means no cutoff is applied."));
 


    // OPTIMISATION PARAMETERS
    //-------------------------------------------------------------------------

    options -> addOption(Int64Option("sa-seed")
                         .store(&saRandomSeed_)
                         .storeIsSet(&saRandomSeedIsSet_)
                         .description("Seed used in pseudo random number "
                                      "generation for simulated annealing. "
                                      "If not set explicitly, a random seed "
                                      "is used."));

    options -> addOption(IntegerOption("sa-max-iter")
                          .store(&saMaxCoolingIter_)
                          .defaultValue(0)
                          .description("Maximum number of cooling iterations "
                                       "in one simulated annealing run."));
                          
    options -> addOption(RealOption("sa-init-temp")
                         .store(&pfPar_["saInitTemp"])
                         .defaultValue(0.1)
                         .description("Simulated annealing initial "
                                      "temperature."));

    options -> addOption(RealOption("sa-cooling-fac")
                         .store(&pfPar_["saCoolingFactor"])
                         .defaultValue(0.98)
                         .description("Simulated annealing cooling factor."));

    options -> addOption(RealOption("sa-step")
                         .store(&pfPar_["saStepLengthFactor"])
                         .defaultValue(0.001)
                         .description("Step length factor used in candidate "
                                      "generation. Defaults to 0.001."));

    options -> addOption(IntegerOption("nm-max-iter")
                         .store(&nmMaxIter_)
                         .defaultValue(100)
                         .description("Number of Nelder-Mead simplex "
                                      "iterations in path finding algorithm."));

    options -> addOption(RealOption("nm-init-shift")
                         .store(&pfPar_["nmInitShift"])
                         .defaultValue(0.1)
                         .description("Distance of vertices in initial "
                                      "Nelder-Mead simplex."));


    // PATH MAPPING PARAMETERS
    //-------------------------------------------------------------------------

    options -> addOption(RealOption("pm-pl-margin")
	                     .store(&poreMappingMargin_)
                         .defaultValue(0.5)
                         .description("Margin for determining pathway lining "
                                      "residues. A residue is considered to "
                                      "be pathway lining if it is no further "
                                      "than the local path radius plus this "
                                      "margin from the pathway's centre "
                                      "line."));


    // DENSITY ESTIMATION PARAMETERS
    //-------------------------------------------------------------------------

    const char * const allowedDensityEstimationMethod[] = {"histogram",
                                                           "kernel"};
    deMethod_ = eDensityEstimatorKernel;
    options -> addOption(EnumOption<eDensityEstimator>("de-method")
                         .enumValue(allowedDensityEstimationMethod)
                         .store(&deMethod_)
                         .description("Method used for estimating the "
                                      "probability density of the solvent "
                                      "particles along the permeation "
                                      "pathway"));
    
    options -> addOption(RealOption("de-res")
                         .store(&deResolution_)
                         .defaultValue(0.01)
                         .description("Spatial resolution of the density "
                                      "estimator. In case of a histogram, "
                                      "this is the bin width, in case of a "
                                      "kernel density estimator, this is the "
                                      "spacing of the evaluation points."));

    options -> addOption(RealOption("de-bandwidth")
                         .store(&deBandWidth_)
                         .defaultValue(-1.0)
                         .description("Bandwidth for the kernel density "
                                      "estimator. Ignored for other "
                                      "methods. If negative or zero, bandwidth"
                                      " will be determined automatically "
                                      "to minimise the asymptotic mean "
                                      "integrated squared error (AMISE)."));

    options -> addOption(RealOption("de-bw-scale")
                         .store(&deBandWidthScale_)
                         .defaultValue(1.0)
                         .description("Scaling factor for the band width."
                                      "Useful to set a bandwidth relative to "
                                      "the AMISE-optimal value."));

    options -> addOption(RealOption("de-eval-cutoff")
                         .store(&deEvalRangeCutoff_)
                         .defaultValue(5)
                         .description("Evaluation range cutoff for kernel "
                                      "density estimator in multiples of "
                                      "bandwidth. Ignored for other methods. "
                                      "Ensures that the density falls off "
                                      "smoothly to zero outside the data "
                                      "range."));


    // HYDROPHOBICITY PARAMETERS
    //-------------------------------------------------------------------------
    
    const char * const allowedHydrophobicityDatabase[] = {"hessa_2005",
                                                          "kyte_doolittle_1982",
                                                          "monera_1995",
                                                          "moon_2011",
                                                          "wimley_white_1996",
                                                          "zhu_2016",
                                                          "memprotmd",
                                                          "user"};
    hydrophobicityDatabase_ = eHydrophobicityDatabaseWimleyWhite1996;
    options -> addOption(EnumOption<eHydrophobicityDatabase>("hydrophob-database")
                         .enumValue(allowedHydrophobicityDatabase)
                         .store(&hydrophobicityDatabase_)
                         .description("Database of hydrophobicity scale for "
                                      "pore forming residues"));

    options -> addOption(RealOption("hydrophob-fallback")
                         .store(&hydrophobicityDefault_)
                         .storeIsSet(&hydrophobicityDefaultIsSet_)
                         .defaultValue(std::nan(""))
                         .description("Fallback hydrophobicity for residues "
                                      "in the pathway defining group. If "
                                      "unset (nan), residues missing in the "
                                      "database will cause an error."));

    options -> addOption(StringOption("hydrophob-json")
                         .store(&hydrophobicityJson_)
                         .storeIsSet(&hydrophobicityJsonIsSet_)
                         .description("JSON file with user defined "
                                      "hydrophobicity scale. Will be "
                                      "ignored unless -hydrophobicity-database"
                                      " is set to 'user'."));
    
    options -> addOption(RealOption("hydrophob-bandwidth")
                         .store(&hpBandWidth_)
                         .defaultValue(0.35)
                         .description("Bandwidth for hydrophobicity kernel."));
}




/*
 * 
 */
void
trajectoryAnalysis::initAnalysis(const TrajectoryAnalysisSettings& /*settings*/,
                                 const TopologyInformation &top)
{
    // save atom coordinates in topology for writing to output later:
    outputStructure_.fromTopology(top);

    // ADD PROPER EXTENSIONS TO FILE NAMES
    //-------------------------------------------------------------------------

    outputJsonFileName_ = outputBaseFileName_ + ".json";
    outputObjFileName_ = outputBaseFileName_ + ".obj";
    outputPdbFileName_ = outputBaseFileName_ + ".pdb";


    // PATH FINDING PARAMETERS
    //-------------------------------------------------------------------------

    // set inut-dependent defaults:
    if( !saRandomSeedIsSet_ )
    {
        saRandomSeed_ = gmx::makeRandomSeed();
    }

    // set parameters in map:
    pfPar_["pfProbeMaxSteps"] = pfMaxProbeSteps_;

    pfPar_["pfCylRad"] = pfMaxProbeRadius_;
    pfPar_["pfCylNumSteps"] = pfPar_["pfProbeMaxSteps"];
    pfPar_["pfCylStepLength"] = pfProbeStepLength_;

    pfPar_["saMaxCoolingIter"] = saMaxCoolingIter_;
    pfPar_["saRandomSeed"] = saRandomSeed_;
    pfPar_["saNumCostSamples"] = saNumCostSamples_;

    pfPar_["nmMaxIter"] = nmMaxIter_;


    // 
    pfParams_.setProbeStepLength(pfProbeStepLength_);
    pfParams_.setMaxProbeRadius(pfMaxProbeRadius_);
    pfParams_.setMaxProbeSteps(pfMaxProbeSteps_);
    
    if( cutoffIsSet_ )
    {
        pfParams_.setNbhCutoff(cutoff_);
    }

    // PATH MAPPING PARAMETERS
    //-------------------------------------------------------------------------

    // sanity checks and automatic defaults:
    if( mappingParams_.mapTol_ <= 0.0 )
    {
        throw(std::runtime_error("Mapping tolerance parameter pm-tol must be positive."));
    }

    if( mappingParams_.extrapDist_ <= 0 )
    {
        throw(std::runtime_error("Extrapolation distance set with pm-extrap-dist may not be negative."));
    }

    if( mappingParams_.sampleStep_ <= 0 )
    {
        throw(std::runtime_error("Sampling step set with pm-sample-step must be positive."));
    }


    // DENSITY ESTIMATION PARAMETERS
    //-------------------------------------------------------------------------

    // which estimator will be used?
    if( deMethod_ == eDensityEstimatorHistogram )
    {
        deParams_.setBinWidth(deResolution_);
    }
    else if( deMethod_ == eDensityEstimatorKernel )
    {
        deParams_.setKernelFunction(eKernelFunctionGaussian);
        deParams_.setBandWidth(deBandWidth_);
        deParams_.setBandWidthScale(deBandWidthScale_);
        deParams_.setEvalRangeCutoff(deEvalRangeCutoff_);
        deParams_.setMaxEvalPointDist(deResolution_);
    }

    
    // HYDROPHOBICITY PARAMETERS
    //-------------------------------------------------------------------------

    // parameters for the hydrophobicity kernel:
    hpResolution_ = deResolution_;
    hpEvalRangeCutoff_ = deEvalRangeCutoff_;
    hydrophobKernelParams_.setKernelFunction(eKernelFunctionGaussian);
    hydrophobKernelParams_.setBandWidth(hpBandWidth_);
    hydrophobKernelParams_.setEvalRangeCutoff(hpEvalRangeCutoff_);
    hydrophobKernelParams_.setMaxEvalPointDist(hpResolution_);


    // PREPARE DATSETS
    //-------------------------------------------------------------------------

    // prepare per frame data stream:
    frameStreamData_.setDataSetCount(9);
    std::vector<std::string> frameStreamDataSetNames = {
            "pathSummary",
            "molPathOrigPoints",
            "molPathRadiusSpline",
            "molPathCentreLineSpline",
            "residuePositions",
            "solventPositions",
            "solventDensitySpline",
            "plHydrophobicitySpline",
            "pfHydrophobicitySpline"};
    std::vector<std::vector<std::string>> frameStreamColumnNames;


    // prepare container for aggregated data:
    frameStreamData_.setColumnCount(0, 14);
    frameStreamColumnNames.push_back({"timeStamp",
                                      "argMinRadius",
                                      "minRadius",
                                      "length",
                                      "volume",
                                      "numPath",
                                      "numSample",
                                      "solventRangeLo",
                                      "solventRangeHi",
                                      "argMinSolventDensity",
                                      "minSolventDensity",
                                      "arcLengthLo",
                                      "arcLengthHi",
                                      "bandWidth"});

    // prepare container for original path points:
    frameStreamData_.setColumnCount(1, 4);
    frameStreamColumnNames.push_back({"x", 
                                      "y",
                                      "z",
                                      "r"});

    // prepare container for path radius:
    frameStreamData_.setColumnCount(2, 2);
    frameStreamColumnNames.push_back({"knots", 
                                      "ctrl"});

    // prepare container for pathway spline:
    frameStreamData_.setColumnCount(3, 4);
    frameStreamColumnNames.push_back({"knots", 
                                      "ctrlX",
                                      "ctrlY",
                                      "ctrlZ"});

    // prepare container for residue mapping results:
    frameStreamData_.setColumnCount(4, 11);
    frameStreamColumnNames.push_back({"resId",
                                      "s",
                                      "rho",
                                      "phi",
                                      "poreLining",
                                      "poreFacing",
                                      "poreRadius",
                                      "solventDensity",
                                      "x",
                                      "y",
                                      "z"});

    // prepare container for solvent mapping:
    frameStreamData_.setColumnCount(5, 9);
    frameStreamColumnNames.push_back({"resId", 
                                      "s",
                                      "rho",
                                      "phi",
                                      "inPore",
                                      "inSample",
                                      "x",
                                      "y",
                                      "z"});

    // prepare container for solvent density:
    frameStreamData_.setColumnCount(6, 2);
    frameStreamColumnNames.push_back({"knots", 
                                      "ctrl"});

    // prepare container for hydrophobicity splines:
    frameStreamData_.setColumnCount(7, 2);
    frameStreamColumnNames.push_back({"knots", 
                                      "ctrl"});
    frameStreamData_.setColumnCount(8, 2);
    frameStreamColumnNames.push_back({"knots", 
                                      "ctrl"});

    // add JSON exporter to frame stream data:
    AnalysisDataJsonFrameExporterPointer jsonFrameExporter(new AnalysisDataJsonFrameExporter);
    jsonFrameExporter -> setDataSetNames(frameStreamDataSetNames);
    jsonFrameExporter -> setColumnNames(frameStreamColumnNames);
    std::string frameStreamFileName = std::string("stream_") + outputJsonFileName_;
    jsonFrameExporter -> setFileName(frameStreamFileName);
    frameStreamData_.addModule(jsonFrameExporter);


    // TIMING DATA
    //-------------------------------------------------------------------------

    // TODO: perhaps mive to constructor?
    timingData_.setDataSetCount(1);
    timingData_.setColumnCount(0, 1);
    timingData_.setMultipoint(false);


    // PREPARE SELECTIONS FOR PORE PARTICLE MAPPING
    //-------------------------------------------------------------------------

    // prepare a centre of geometry selection collection:
    poreMappingSelCol_.setReferencePosType("res_cog");
    poreMappingSelCol_.setOutputPosType("res_cog");
  
    // selection of C-alpha atoms:
    // TODO: this will not work if only part of protein is specified as pore
    std::string refselSelText = refsel_.selectionText();
    std::string poreMappingSelCalString = "name CA";
    std::string poreMappingSelCogString = refselSelText;


    // create index groups from topology:
    // TODO: this will probably not work for custom index groups
    gmx_ana_indexgrps_t *poreIdxGroups;
    gmx_ana_indexgrps_init(&poreIdxGroups, 
                           top.topology(), 
                           NULL); 

    // create selections as defined above:
    poreMappingSelCal_ = poreMappingSelCol_.parseFromString(poreMappingSelCalString)[0];
    poreMappingSelCog_ = poreMappingSelCol_.parseFromString(poreMappingSelCogString)[0];
    poreMappingSelCol_.setTopology(top.topology(), 0);
    poreMappingSelCol_.setIndexGroups(poreIdxGroups);
    poreMappingSelCol_.compile();

    // free memory:
    gmx_ana_indexgrps_free(poreIdxGroups);

    // validate that there is a c-alpha for each residue:
    if( poreMappingSelCal_.posCount() != poreMappingSelCog_.posCount() )
    {
        std::cerr<<"ERROR: Could not find a C-alpha for each residue in pore forming group."
                 <<std::endl<<"Is your pore a protein?"<<std::endl;
        std::abort();
    }


    // PREPARE SELECTIONS FOR SOLVENT PARTICLE MAPPING
    //-------------------------------------------------------------------------

    // only do this if solvent selection specified:
    if( !sel_.empty() )
    {
        // prepare centre of geometry selection collection:
        solvMappingSelCol_.setReferencePosType("res_cog");
        solvMappingSelCol_.setOutputPosType("res_cog");

        // create index groups from topology:
        // TODO: this will not work for custom index groups
        gmx_ana_indexgrps_t *solvIdxGroups;
        gmx_ana_indexgrps_init(&solvIdxGroups,
                               top.topology(),
                               NULL);

        // selection text:
        std::string solvMappingSelCogString = sel_[0].selectionText();

        // create selection as defined by user:
        solvMappingSelCog_ = solvMappingSelCol_.parseFromString(solvMappingSelCogString)[0];

        // compile the selections:
        solvMappingSelCol_.setTopology(top.topology(), 0);
        solvMappingSelCol_.setIndexGroups(solvIdxGroups);
        solvMappingSelCol_.compile();

        // free memory:
        gmx_ana_indexgrps_free(solvIdxGroups);
    }

    
    // PREPARE TOPOLOGY QUERIES
    //-------------------------------------------------------------------------

	// load full topology:
	t_topology *topol = top.topology();	

	// access list of all atoms:
	t_atoms atoms = topol -> atoms;
    
	// create atomprop struct:
	gmx_atomprop_t aps = gmx_atomprop_init();

    
    // GET ATOM RADII FROM TOPOLOGY
    //-------------------------------------------------------------------------

    // get location of program binary from program context:
    const gmx::IProgramContext &programContext = gmx::getProgramContext();
    std::string radiusFilePath = programContext.fullBinaryPath();

    // obtain radius database location as relative path:
    auto lastSlash = radiusFilePath.find_last_of('/');
    radiusFilePath.replace(radiusFilePath.begin() + lastSlash - 5, 
                           radiusFilePath.end(), 
                           "share/data/vdwradii/");

    radiusFilePath = chapInstallBase() + std::string("/share/data/vdwradii/");
    

    // select appropriate database file:
    if( pfVdwRadiusDatabase_ == eVdwRadiusDatabaseHoleAmberuni )
    {
        pfVdwRadiusJson_ = radiusFilePath + "hole_amberuni.json";
    }
    else if( pfVdwRadiusDatabase_ == eVdwRadiusDatabaseHoleBondi )
    {
        pfVdwRadiusJson_ = radiusFilePath + "hole_bondi.json";
    }
    else if( pfVdwRadiusDatabase_ == eVdwRadiusDatabaseHoleHardcore )
    {
        pfVdwRadiusJson_ = radiusFilePath + "hole_hardcore.json";
    }
    else if( pfVdwRadiusDatabase_ == eVdwRadiusDatabaseHoleSimple )
    {
        pfVdwRadiusJson_ = radiusFilePath + "hole_simple.json";
    }
    else if( pfVdwRadiusDatabase_ == eVdwRadiusDatabaseHoleXplor )
    {
        pfVdwRadiusJson_ = radiusFilePath + "hole_xplor.json";
    }
    else if( pfVdwRadiusDatabase_ == eVdwRadiusDatabaseUser )
    {
        // has user provided a file name?
        if( !pfVdwRadiusJsonIsSet_ )
        {
            throw std::runtime_error("ERROR: Option pfVdwRadiusDatabase set "
                                     "to 'user', but no custom van-der-Waals "
                                     "radii specified with pfVdwRadiusJson.");
        }
    }

    // import vdW radii JSON: 
    JsonDocImporter jdi;
    rapidjson::Document radiiDoc = jdi(pfVdwRadiusJson_.c_str());
   
    // create radius provider and build lookup table:
    VdwRadiusProvider vrp;
    try
    {
        vrp.lookupTableFromJson(radiiDoc);
    }
    catch( std::exception& e )
    {
        std::cerr<<"ERROR while creating van der Waals radius lookup table:"<<std::endl;
        std::cerr<<e.what()<<std::endl; 
        std::abort();
    }


    // TRACK C-ALPHAS and RESIDUE INDICES
    //-------------------------------------------------------------------------
   
    // loop through all atoms, get index lists for c-alphas and residues:
    for(int i = 0; i < atoms.nr; i++)
    {
        // check for calpha:
        if( std::strcmp(*atoms.atomname[i], "CA") == 0 )
        {
           poreCAlphaIndices_.push_back(i); 
        }

        // track residue ID of atoms: 
        residueIndices_.push_back(atoms.atom[i].resind);
        atomResidueMapping_[i] = atoms.atom[i].resind;
        residueAtomMapping_[atoms.atom[i].resind].push_back(i);
    }

    // remove duplicate residue indices:
    std::vector<int>::iterator it;
    it = std::unique(residueIndices_.begin(), residueIndices_.end());
    residueIndices_.resize(std::distance(residueIndices_.begin(), it));

    // loop over residues:
    ConstArrayRef<int> refselAtomIdx = refsel_.atomIndices();
    for(it = residueIndices_.begin(); it != residueIndices_.end(); it++)
    {
        // current residue id:
        int resId = *it;
    
        // get vector of all atom indices in this residue:
        std::vector<int> atomIdx = residueAtomMapping_[resId];

        // for each atom in residue, check if it belongs to pore selection:
        bool addResidue = false;
        std::vector<int>::iterator jt;
        for(jt = atomIdx.begin(); jt != atomIdx.end(); jt++)
        {
            // check if atom belongs to pore selection:
            if( std::find(refselAtomIdx.begin(), refselAtomIdx.end(), *jt) != refselAtomIdx.end() )
            {
                // add atom to list of pore atoms:
                poreAtomIndices_.push_back(*jt);

                // if at least one atom belongs to pore group, the whole residue will be considered:
                addResidue = true;
            }
        }

        // add residue, if at least one atom is part of pore:
        if( addResidue == true )
        {
            poreResidueIndices_.push_back(resId);
        }
    }


    // FINALISE ATOMPROP QUERIES
    //-------------------------------------------------------------------------
    
	// delete atomprop struct:
	gmx_atomprop_destroy(aps);

    // set user-defined default radius?
    if( pfDefaultVdwRadiusIsSet_ )
    {
        vrp.setDefaultVdwRadius(pfDefaultVdwRadius_);
    }

    // build vdw radius lookup map:
    try
    {
        vdwRadii_ = vrp.vdwRadiiForTopology(top, refsel_.mappedIds());
    }
    catch( std::exception& e )
    {
        std::cerr<<"ERROR in van der Waals radius lookup:"<<std::endl;
        std::cerr<<e.what()<<std::endl;
        std::abort();
    } 

    // find maximum van der Waals radius:
    maxVdwRadius_ = std::max_element(vdwRadii_.begin(), vdwRadii_.end()) -> second;


    // GET RESIDUE CHEMICAL INFORMATION
    //-------------------------------------------------------------------------

    // get residue information from topology:
    resInfo_.nameFromTopology(top);
    resInfo_.chainFromTopology(top);

    // base path to location of hydrophobicity databases:
    std::string hydrophobicityFilePath = chapInstallBase() + 
            std::string("/share/data/hydrophobicity/");
    
    // select appropriate database file:
    if( hydrophobicityDatabase_ == eHydrophobicityDatabaseHessa2005 )
    {
        hydrophobicityJson_ = hydrophobicityFilePath + "hessa_2005.json";
    }
    else if( hydrophobicityDatabase_ == eHydrophobicityDatabaseKyteDoolittle1982 )
    {
        hydrophobicityJson_ = hydrophobicityFilePath + "kyte_doolittle_1982.json";
    }
    else if( hydrophobicityDatabase_ == eHydrophobicityDatabaseMonera1995 )
    {
        hydrophobicityJson_ = hydrophobicityFilePath + "monera_1995.json";
    }
    else if( hydrophobicityDatabase_ == eHydrophobicityDatabaseMoon2011 )
    {
        hydrophobicityJson_ = hydrophobicityFilePath + "moon_2011.json";
    }
    else if( hydrophobicityDatabase_ == eHydrophobicityDatabaseWimleyWhite1996 )
    {
        hydrophobicityJson_ = hydrophobicityFilePath + "wimley_white_1996.json";
    }
    else if( hydrophobicityDatabase_ == eHydrophobicityDatabaseZhu2016 )
    {
        hydrophobicityJson_ = hydrophobicityFilePath + "zhu_2016.json";
    }
    else if( hydrophobicityDatabase_ == eHydrophobicityDatabaseMemprotMd )
    {
        hydrophobicityJson_ = hydrophobicityFilePath + "memprotmd.json";
    }
    else if( hydrophobicityDatabase_ == eHydrophobicityDatabaseUser )
    {
        // has user provided a file name?
        if( !hydrophobicityJsonIsSet_ )
        {
            std::cerr<<"ERROR: Option hydrophob-database set to 'user', but "
            "no custom hydrophobicity scale was specified with "
            "hydrophob-json."<<std::endl;
            std::abort();
        }
    }

    // import hydrophbicity JSON:
    rapidjson::Document hydrophobicityDoc = jdi(hydrophobicityJson_.c_str());
   
    // generate hydrophobicity lookup table:
    resInfo_.hydrophobicityFromJson(hydrophobicityDoc);

    // set fallback hydrophobicity:
    if( hydrophobicityDefaultIsSet_ )
    {
        resInfo_.setDefaultHydrophobicity(hydrophobicityDefault_);
    }

    // free line for nice output:
    std::cout<<std::endl;
}


/*!
 *
 */
void
trajectoryAnalysis::initAfterFirstFrame(
        const TrajectoryAnalysisSettings &settings,
        const t_trxframe &fr)
{

}


/*
 *
 */
void
trajectoryAnalysis::analyzeFrame(
        int frnr, 
        const t_trxframe &fr, 
        t_pbc *pbc,
        TrajectoryAnalysisModuleData *pdata)
{

    // get thread-local selections:
	const Selection &refSelection = pdata -> parallelSelection(refsel_);
//    const Selection &initProbePosSelection = pdata -> parallelSelection(initProbePosSelection_);

    // get data handles for this frame:
    AnalysisDataHandle dhFrameStream = pdata -> dataHandle(frameStreamData_);
    AnalysisDataHandle dhTiming = pdata -> dataHandle(timingData_);

	// get data for frame number frnr into data handle:
    dhFrameStream.startFrame(frnr, fr.time);
    dhTiming.startFrame(frnr, fr.time);


    // UPDATE INITIAL PROBE POSITION FOR THIS FRAME
    //-------------------------------------------------------------------------

    // recalculate initial probe position based on reference group COG:
    if( pfInitProbePosIsSet_ == false )
    {  
        // helper variable for conditional assignment of selection:
        Selection tmpsel;
  
        // has a group for specifying initial probe position been set?
        if( ippselIsSet_ == true )
        {
            // use explicitly given selection:
            tmpsel = ippsel_;
        }
        else 
        {
            // default to overall group of pore forming particles:
            tmpsel = refsel_;
        }
     
        // load data into initial position selection:
        const gmx::Selection &initPosSelection = pdata -> parallelSelection(tmpsel);
 
        // initialse total mass and COM vector:
        real totalMass = 0.0;
        gmx::RVec centreOfMass(0.0, 0.0, 0.0);
        
        // loop over all atoms: 
        for(int i = 0; i < initPosSelection.atomCount(); i++)
        {
            // get i-th atom position:
            gmx::SelectionPosition atom = initPosSelection.position(i);

            // add to total mass:
            totalMass += atom.mass();

            // add to COM vector:
            // TODO: implement separate centre of geometry and centre of mass 
            centreOfMass[0] += atom.mass() * atom.x()[0];
            centreOfMass[1] += atom.mass() * atom.x()[1];
            centreOfMass[2] += atom.mass() * atom.x()[2];
        }

        // scale COM vector by total MASS:
        centreOfMass[0] /= 1.0 * totalMass;
        centreOfMass[1] /= 1.0 * totalMass;
        centreOfMass[2] /= 1.0 * totalMass; 

        // set initial probe position:
        pfInitProbePos_[0] = centreOfMass[0];
        pfInitProbePos_[1] = centreOfMass[1];
        pfInitProbePos_[2] = centreOfMass[2];
    }


    // GET VDW RADII FOR SELECTION
    //-------------------------------------------------------------------------
    // TODO: Move this to separate class and test!
    // TODO: Should then also work for coarse-grained situations!

	// create vector of van der Waals radii and allocate memory:
    std::vector<real> selVdwRadii;
	selVdwRadii.reserve(refSelection.atomCount());

    // loop over all atoms in system and get vdW-radii:
	for(int i=0; i<refSelection.atomCount(); i++)
    {
        // get global index of i-th atom in selection:
        gmx::SelectionPosition atom = refSelection.position(i);
        int idx = atom.mappedId();

		// add radius to vector of radii:
		selVdwRadii.push_back(vdwRadii_.at(idx));
	}


	// PORE FINDING AND RADIUS CALCULATION
	// ------------------------------------------------------------------------

    // vectors as RVec:
    RVec initProbePos(pfInitProbePos_[0], pfInitProbePos_[1], pfInitProbePos_[2]);
    RVec chanDirVec(pfChanDirVec_[0], pfChanDirVec_[1], pfChanDirVec_[2]); 

    // create path finding module:
    std::unique_ptr<AbstractPathFinder> pfm;
    if( pfMethod_ == ePathFindingMethodInplaneOptimised )
    {
        // create inplane-optimised path finder:
        pfm.reset(new InplaneOptimisedProbePathFinder(pfPar_,
                                                      initProbePos,
                                                      chanDirVec,
                                                      pbc,
                                                      refSelection,
                                                      selVdwRadii));        
    }
    else if( pfMethod_ == ePathFindingMethodNaiveCylindrical )
    {        
        // create the naive cylindrical path finder:
        pfm.reset(new NaiveCylindricalPathFinder(pfPar_,
                                                 initProbePos,
                                                 chanDirVec));
    }

    // set parameters:
    pfm -> setParameters(pfParams_);


    // PATH FINDING
    //-------------------------------------------------------------------------

    // run path finding algorithm on current frame:
    std::cout.flush();
    clock_t tPathFinding = std::clock();
    pfm -> findPath();
    tPathFinding = (std::clock() - tPathFinding)/CLOCKS_PER_SEC;

    // retrieve molecular path object:
    std::cout.flush();
    clock_t tMolPath = std::clock();
    MolecularPath molPath = pfm -> getMolecularPath();
    tMolPath = (std::clock() - tMolPath)/CLOCKS_PER_SEC;
    
    // which method do we use for path alignment?
    if( pfPathAlignmentMethod_ == ePathAlignmentMethodNone )
    {
        // no need to do anything in this case
    }
    else if( pfPathAlignmentMethod_ == ePathAlignmentMethodIpp )
    {
        // map initial probe position onto pathway:
        std::vector<gmx::RVec> ipp;
        ipp.push_back(initProbePos);
        std::vector<gmx::RVec> mappedIpp = molPath.mapPositions(
                ipp, 
                mappingParams_);

        // shift coordinates of molecular path appropriately:
        molPath.shift(mappedIpp.front());
    }

    // get original path points and radii:
    std::vector<gmx::RVec> pathPoints = molPath.pathPoints();
    std::vector<real> pathRadii = molPath.pathRadii();

    // add original path points to frame stream dataset:
    dhFrameStream.selectDataSet(1);
    for(size_t i = 0; i < pathPoints.size(); i++)
    {
        dhFrameStream.setPoint(0, pathPoints.at(i)[XX]);
        dhFrameStream.setPoint(1, pathPoints.at(i)[YY]);
        dhFrameStream.setPoint(2, pathPoints.at(i)[ZZ]);
        dhFrameStream.setPoint(3, pathRadii.at(i));
        dhFrameStream.finishPointSet();
    }

    // add radius spline knots and control points to frame stream dataset:
    dhFrameStream.selectDataSet(2);
    std::vector<real> radiusKnots = molPath.poreRadiusUniqueKnots();    
    std::vector<real> radiusCtrlPoints = molPath.poreRadiusCtrlPoints();
    for(size_t i = 0; i < radiusKnots.size(); i++)
    {
        dhFrameStream.setPoint(0, radiusKnots.at(i));
        dhFrameStream.setPoint(1, radiusCtrlPoints.at(i));
        dhFrameStream.finishPointSet();
    }
    
    // add centre line spline knots and control points to frame stream dataset:
    dhFrameStream.selectDataSet(3);
    std::vector<real> centreLineKnots = molPath.centreLineUniqueKnots();    
    std::vector<gmx::RVec> centreLineCtrlPoints = molPath.centreLineCtrlPoints();
    for(size_t i = 0; i < centreLineKnots.size(); i++)
    {
        dhFrameStream.setPoint(0, centreLineKnots.at(i));
        dhFrameStream.setPoint(1, centreLineCtrlPoints.at(i)[XX]);
        dhFrameStream.setPoint(2, centreLineCtrlPoints.at(i)[YY]);
        dhFrameStream.setPoint(3, centreLineCtrlPoints.at(i)[ZZ]);
        dhFrameStream.finishPointSet();
    }


    // MAP PORE PARTICLES ONTO PATHWAY
    //-------------------------------------------------------------------------
 
    // evaluate pore mapping selection for this frame:
    t_trxframe frame = fr;
    poreMappingSelCol_.evaluate(&frame, pbc);
    const gmx::Selection poreMappingSelCal = pdata -> parallelSelection(poreMappingSelCal_);    
    const gmx::Selection poreMappingSelCog = pdata -> parallelSelection(poreMappingSelCog_);    


    // map pore residue COG onto pathway:
    clock_t tMapResCog = std::clock();
    std::map<int, gmx::RVec> poreCogMappedCoords = molPath.mapSelection(
            poreMappingSelCog, 
            mappingParams_);
    tMapResCog = (std::clock() - tMapResCog)/CLOCKS_PER_SEC;

    // map pore residue C-alpha onto pathway:
    clock_t tMapResCal = std::clock();
    std::map<int, gmx::RVec> poreCalMappedCoords = molPath.mapSelection(
            poreMappingSelCal, 
            mappingParams_);
    tMapResCal = (std::clock() - tMapResCal)/CLOCKS_PER_SEC;

    
    // check if particles are pore-lining:
    clock_t tResPoreLining = std::clock();
    std::map<int, bool> poreLining = molPath.checkIfInside(
            poreCogMappedCoords, 
            poreMappingMargin_);
    int nPoreLining = 0;
    for(auto jt = poreLining.begin(); jt != poreLining.end(); jt++)
    {
        if( jt -> second == true )
        {
            nPoreLining++;
        }
    }
    tResPoreLining = (std::clock() - tResPoreLining)/CLOCKS_PER_SEC;

    // check if residues are pore-facing:
    // TODO: make this conditional on whether C-alphas are available
    
    clock_t tResPoreFacing = std::clock();
    std::map<int, bool> poreFacing;
    int nPoreFacing = 0;
    for(auto it = poreCogMappedCoords.begin(); it != poreCogMappedCoords.end(); it++)
    {
        // is residue pore lining and has COG closer to centreline than CA?
        if( it -> second[RR] < poreCalMappedCoords[it->first][RR] &&
            poreLining[it -> first] == true )
        {
            poreFacing[it->first] = true;
            nPoreFacing++;
        }
        else
        {
            poreFacing[it->first] = false;            
        }
    }
    tResPoreFacing = (std::clock() - tResPoreFacing)/CLOCKS_PER_SEC;
    

    // ESTIMATE HYDROPHOBICITY PROFILE
    //-------------------------------------------------------------------------
   
    // get vectors of coordinates of pore-facing and -lining residues:
    std::vector<real> plResidueCoordS;
    std::vector<real> plResidueHydrophobicity;
    std::vector<real> pfResidueCoordS;
    std::vector<real> pfResidueHydrophobicity;
    real minPoreResS = std::numeric_limits<real>::infinity();
    real maxPoreResS = -std::numeric_limits<real>::infinity();
    for(auto res : poreCogMappedCoords)
    {
        if( poreLining[res.first] )
        {
            plResidueCoordS.push_back(res.second[SS]);
            plResidueHydrophobicity.push_back(
                    resInfo_.hydrophobicity(res.first));
        }
        if( poreFacing[res.first])
        {
            pfResidueCoordS.push_back(res.second[SS]);
            pfResidueHydrophobicity.push_back(
                    resInfo_.hydrophobicity(res.first));
        }

        // also track the largest and smallest residue positions:
        if( res.second[SS] < minPoreResS )
        {
            minPoreResS = res.second[SS];
        }
        if( res.second[SS] > maxPoreResS )
        {
            maxPoreResS = res.second[SS];
        }
    }

    // add mock values at both ends to ensure profile goes to zero smoothly:
    pfResidueCoordS.push_back(minPoreResS - hpBandWidth_/2.0);
    pfResidueCoordS.push_back(maxPoreResS + hpBandWidth_/2.0);
    pfResidueHydrophobicity.push_back(0.0);
    pfResidueHydrophobicity.push_back(0.0);

    plResidueCoordS.push_back(minPoreResS - hpBandWidth_/2.0);
    plResidueCoordS.push_back(maxPoreResS + hpBandWidth_/2.0);
    plResidueHydrophobicity.push_back(0.0);
    plResidueHydrophobicity.push_back(0.0);

    // set up kernel smoother:
    WeightedKernelDensityEstimator kernelSmoother;
    kernelSmoother.setParameters(hydrophobKernelParams_);

    // estimate hydrophobicity profiles due to pore-lining residues:
    SplineCurve1D plHydrophobicity = kernelSmoother.estimate(
            plResidueCoordS, 
            plResidueHydrophobicity);

    // add spline curve parameters to data handle:   
    dhFrameStream.selectDataSet(7);
    for(size_t i = 0; i < plHydrophobicity.ctrlPoints().size(); i++)
    {
        dhFrameStream.setPoint(
                0, 
                plHydrophobicity.uniqueKnots().at(i));
        dhFrameStream.setPoint(
                1, 
                plHydrophobicity.ctrlPoints().at(i));
        dhFrameStream.finishPointSet();
    }
 
    // estimate hydrophobicity profiles due to pore-facing residues:
    SplineCurve1D pfHydrophobicity = kernelSmoother.estimate(
            pfResidueCoordS, 
            pfResidueHydrophobicity);

    // add spline curve parameters to data handle:   
    dhFrameStream.selectDataSet(8);
    for(size_t i = 0; i < pfHydrophobicity.ctrlPoints().size(); i++)
    {
        dhFrameStream.setPoint(
                0, 
                pfHydrophobicity.uniqueKnots().at(i));
        dhFrameStream.setPoint(
                1, 
                pfHydrophobicity.ctrlPoints().at(i));
        dhFrameStream.finishPointSet();
    }


    // MAP SOLVENT PARTICLES ONTO PATHWAY
    //-------------------------------------------------------------------------

    // create data containers:
    std::map<int, gmx::RVec> solventMappedCoords; 
    std::map<int, bool> solvInsideSample;
    std::map<int, bool> solvInsidePore;
    int numSolvInsideSample = 0;
    int numSolvInsidePore = 0;

    // only do this if solvent selection is valid:
    if( !sel_.empty() )
    {
        // evaluate solevnt mapping selections for this frame:
        t_trxframe tmpFrame = fr;
        solvMappingSelCol_.evaluate(&tmpFrame, pbc);

        // TODO: make this a parameter:
        real solvMappingMargin_ = 0.0;
            
        // get thread-local selection data:
        const Selection solvMapSel = pdata -> parallelSelection(solvMappingSelCog_);

        // map particles onto pathway:
        clock_t tMapSol = std::clock();
        solventMappedCoords = molPath.mapSelection(solvMapSel, mappingParams_);
        tMapSol = (std::clock() - tMapSol)/CLOCKS_PER_SEC;

        // find particles inside path (i.e. pore plus bulk sampling regime):
        clock_t tSolInsideSample = std::clock();
        solvInsideSample = molPath.checkIfInside(
                solventMappedCoords, 
                solvMappingMargin_);
        for(auto jt = solvInsideSample.begin(); jt != solvInsideSample.end(); jt++)
        {            
            if( jt -> second == true )
            {
                numSolvInsideSample++;
            }
        }
        tSolInsideSample = (std::clock() - tSolInsideSample)/CLOCKS_PER_SEC;

        // find particles inside pore:
        clock_t tSolInsidePore = std::clock();
        solvInsidePore = molPath.checkIfInside(
                solventMappedCoords, 
                solvMappingMargin_,
                molPath.sLo(),
                molPath.sHi());
        for(auto jt = solvInsidePore.begin(); jt != solvInsidePore.end(); jt++)
        {            
            if( jt -> second == true )
            {
                numSolvInsidePore++;
            }
        }
        tSolInsidePore = (std::clock() - tSolInsidePore)/CLOCKS_PER_SEC;

        // now add mapped residue coordinates to data handle:
        dhFrameStream.selectDataSet(5);
        
        // add mapped residues to data container:
        for(auto it = solventMappedCoords.begin(); 
            it != solventMappedCoords.end(); 
            it++)
        {
             dhFrameStream.setPoint(0, solvMapSel.position(it -> first).mappedId()); // res.id
             dhFrameStream.setPoint(1, it -> second[0]);     // s
             dhFrameStream.setPoint(2, it -> second[1]);     // rho
             dhFrameStream.setPoint(3, 0.0);     // phi // FIXME wrong, but JSON cant handle NaN
             dhFrameStream.setPoint(4, solvInsidePore[it -> first]);     // inside pore
             dhFrameStream.setPoint(5, solvInsideSample[it -> first]);     // inside sample
             dhFrameStream.setPoint(6, solvMapSel.position(it -> first).x()[0]);  // x
             dhFrameStream.setPoint(7, solvMapSel.position(it -> first).x()[1]);  // y
             dhFrameStream.setPoint(8, solvMapSel.position(it -> first).x()[2]);  // z
             dhFrameStream.finishPointSet();
        }
    }

    
    // ESTIMATE SOLVENT DENSITY
    //-------------------------------------------------------------------------

    // TODO this entire section can easily be made its own class

    // build a vector of sample points inside the pathway:
    std::vector<real> solventSampleCoordS;
    solventSampleCoordS.reserve(solventMappedCoords.size());
    for(auto isInsideSample : solvInsideSample)
    {
        // is this particle inside the pathway?
        if( isInsideSample.second )
        {
            // add arc length coordinate to sample vector:
            solventSampleCoordS.push_back(
                    solventMappedCoords[isInsideSample.first][SS]);
        }
    }

    // sample points inside the por eonly for bandwidth estimation:
    std::vector<real> solventPoreCoordS;
    solventPoreCoordS.reserve(solventMappedCoords.size());
    for(auto isInsidePore : solvInsidePore)
    {
        if( isInsidePore.second )
        {
            solventPoreCoordS.push_back(
                    solventMappedCoords[isInsidePore.first][SS]);
        }
    }

    // create density estimator:
    std::unique_ptr<AbstractDensityEstimator> densityEstimator;
    if( deMethod_ == eDensityEstimatorHistogram )
    {
        densityEstimator.reset(new HistogramDensityEstimator());
    }
    else if( deMethod_ == eDensityEstimatorKernel )
    {
        if( deBandWidth_ <= 0.0 )
        {
            AmiseOptimalBandWidthEstimator bwe;
            deParams_.setBandWidth( bwe.estimate(solventPoreCoordS) );
        }

        densityEstimator.reset(new KernelDensityEstimator());
    }

    // set parameters for density estimation:
    densityEstimator -> setParameters(deParams_);

    // estimate density of solvent particles along arc length coordinate:
    SplineCurve1D solventDensityCoordS = densityEstimator -> estimate(
            solventSampleCoordS);

    // add spline curve parameters to data handle:   
    dhFrameStream.selectDataSet(6);
    for(size_t i = 0; i < solventDensityCoordS.ctrlPoints().size(); i++)
    {
        dhFrameStream.setPoint(
                0, 
                solventDensityCoordS.uniqueKnots().at(i));
        dhFrameStream.setPoint(
                1, 
                solventDensityCoordS.ctrlPoints().at(i));
        dhFrameStream.finishPointSet();
    }

    // track range covered by solvent:
    real solventRangeLo = solventDensityCoordS.uniqueKnots().front();
    real solventRangeHi = solventDensityCoordS.uniqueKnots().back();

    // obtain physical number density:
    SplineCurve1D pathRadius = molPath.pathRadius();
    NumberDensityCalculator ncc;
    SplineCurve1D numberDensity = ncc(
            solventDensityCoordS, 
            pathRadius, 
            numSolvInsideSample);
  
    // find minimum instantaneous solvent density in this frame:
    std::pair<real, real> lim(molPath.sLo(), molPath.sHi());
    std::pair<real, real> minSolventDensity = numberDensity.minimum(lim);


    // ADD AGGREGATE DATA TO PARALLELISABLE CONTAINER
    //-------------------------------------------------------------------------   

    // add aggegate path data:
    dhFrameStream.selectDataSet(0);

    // only one point per frame:
    dhFrameStream.setPoint(0, fr.time);
    dhFrameStream.setPoint(1, molPath.minRadius().first);
    dhFrameStream.setPoint(2, molPath.minRadius().second);
    dhFrameStream.setPoint(3, molPath.length());
    dhFrameStream.setPoint(4, molPath.volume());
    dhFrameStream.setPoint(5, numSolvInsidePore); 
    dhFrameStream.setPoint(6, numSolvInsideSample); 
    dhFrameStream.setPoint(7, solventRangeLo); 
    dhFrameStream.setPoint(8, solventRangeHi);
    dhFrameStream.setPoint(9, minSolventDensity.first); 
    dhFrameStream.setPoint(10, minSolventDensity.second);
    dhFrameStream.setPoint(11, molPath.sLo()); 
    dhFrameStream.setPoint(12, molPath.sHi());
    dhFrameStream.setPoint(13, deParams_.bandWidth()*deParams_.bandWidthScale());
    dhFrameStream.finishPointSet();


    // ADD RESIDUE DATA TO CONTAINER
    //-------------------------------------------------------------------------

    // get pore radius and solvent density at each residue's position:
    std::map<int, real> poreRadiusAtResidue;
    std::map<int, real> solventDensityAtResidue;
    for(auto res : poreCogMappedCoords)
    {
        // get residue-local radius and density:
        real rad = molPath.radius(res.second[SS]);
        real den = solventDensityCoordS.evaluate(res.second[SS], 0);

        // add radius and density to data handle:
        poreRadiusAtResidue[res.first] = rad;
        solventDensityAtResidue[res.first] = den;
    }

    // add mapped residues to data container:
    dhFrameStream.selectDataSet(4);
    for(auto it = poreCogMappedCoords.begin(); it != poreCogMappedCoords.end(); it++)
    {
        dhFrameStream.setPoint( 0, poreMappingSelCog.position(it -> first).mappedId());
        dhFrameStream.setPoint( 1, it -> second[SS]);            // s
        dhFrameStream.setPoint( 2, std::sqrt(it -> second[RR])); // rho
        dhFrameStream.setPoint( 3, it -> second[PP]);            // phi
        dhFrameStream.setPoint( 4, poreLining[it -> first]);     // pore lining?
        dhFrameStream.setPoint( 5, poreFacing[it -> first]);     // pore facing?
        dhFrameStream.setPoint( 6, poreRadiusAtResidue[it -> first]);
        dhFrameStream.setPoint( 7, solventDensityAtResidue[it -> first]);
        dhFrameStream.setPoint( 8, poreMappingSelCog.position(it -> first).x()[XX]);
        dhFrameStream.setPoint( 9, poreMappingSelCog.position(it -> first).x()[YY]);
        dhFrameStream.setPoint(10, poreMappingSelCog.position(it -> first).x()[ZZ]);
        dhFrameStream.finishPointSet();
    }


    // WRITE PORE TO OBJ FILE
    //-------------------------------------------------------------------------

    // TODO: this should be moved to a separate binary!

    MolecularPathObjExporter molPathExp;
    molPathExp(outputObjFileName_.c_str(),
               molPath);


    // ADD TIMING DATA TO DATA HANDLE
    //-------------------------------------------------------------------------

    dhTiming.selectDataSet(0);
    dhTiming.setPoint(0, 1.1111);


    // FINISH FRAME
    //-------------------------------------------------------------------------

	// finish analysis of current frame:
    dhFrameStream.finishFrame();
    dhTiming.finishFrame();
}



/*
 *
 */
void
trajectoryAnalysis::finishAnalysis(int numFrames)
{
    // free line for neater output:
    std::cout<<std::endl;

    // transfer file names from user input:
    std::string inFileName = std::string("stream_") + outputJsonFileName_;
    std::string outFileName = outputJsonFileName_;
    std::fstream inFile;
    std::fstream outFile;

    // READ PER-FRAME DATA AND AGGREGATE ALL NON-PROFILE DATA
    // ------------------------------------------------------------------------

    // openen per-frame data set for reading:
    inFile.open(inFileName, std::fstream::in);

    // prepare summary statistics for aggregate properties:
    SummaryStatistics argMinRadiusSummary;
    SummaryStatistics minRadiusSummary;
    SummaryStatistics lengthSummary;
    SummaryStatistics volumeSummary;
    SummaryStatistics numPathSummary;
    SummaryStatistics numSampleSummary;
    SummaryStatistics solventRangeLoSummary;
    SummaryStatistics solventRangeHiSummary;
    SummaryStatistics argMinSolventDensitySummary;
    SummaryStatistics minSolventDensitySummary;
    SummaryStatistics arcLengthLoSummary;
    SummaryStatistics arcLengthHiSummary;
    SummaryStatistics bandWidthSummary;

    // containers for scalar time series:
    std::vector<real> argMinRadiusTimeSeries;
    std::vector<real> minRadiusTimeSeries;
    std::vector<real> lengthTimeSeries;
    std::vector<real> volumeTimeSeries;
    std::vector<real> numPathwayTimeSeries;
    std::vector<real> numSampleTimeSeries;
    std::vector<real> argMinSolventDensityTimeSeries;
    std::vector<real> minSolventDensityTimeSeries;
    std::vector<real> bandWidthTimeSeries;

    // number of residues in pore forming group:
    size_t numPoreRes = 0;
    std::vector<int> poreResIds;

    // container for time stamps:
    std::vector<real> timeStamps;

    // read file line by line and calculate summary statistics:
    int linesRead = 0;
    std::string line;
    while( std::getline(inFile, line) )
    {
        // read line into JSON document:
        rapidjson::StringStream lineStream(line.c_str());
        rapidjson::Document lineDoc;
        lineDoc.ParseStream(lineStream);

        // sanity checks:
        if( !lineDoc.IsObject() )
        {
            // FIXME this is where the JSON error occurs
            std::string error = "Line " + std::to_string(linesRead) + 
            " read from" + inFileName + " is not valid JSON object.";
            throw std::runtime_error(error);
        }
    
        // calculate summary statistics of aggregate variables:
        argMinRadiusSummary.update(
                lineDoc["pathSummary"]["argMinRadius"][0].GetDouble());
        minRadiusSummary.update(
                lineDoc["pathSummary"]["minRadius"][0].GetDouble());
        lengthSummary.update(
                lineDoc["pathSummary"]["length"][0].GetDouble());
        volumeSummary.update(
                lineDoc["pathSummary"]["volume"][0].GetDouble());
        numPathSummary.update(
                lineDoc["pathSummary"]["numPath"][0].GetDouble());
        numSampleSummary.update(
                lineDoc["pathSummary"]["numSample"][0].GetDouble());
        solventRangeLoSummary.update(
                lineDoc["pathSummary"]["solventRangeLo"][0].GetDouble());
        solventRangeHiSummary.update(
                lineDoc["pathSummary"]["solventRangeHi"][0].GetDouble());
        argMinSolventDensitySummary.update(
                lineDoc["pathSummary"]["argMinSolventDensity"][0].GetDouble());
        minSolventDensitySummary.update(
                lineDoc["pathSummary"]["minSolventDensity"][0].GetDouble());
        arcLengthLoSummary.update(
                lineDoc["pathSummary"]["arcLengthLo"][0].GetDouble());
        arcLengthHiSummary.update(
                lineDoc["pathSummary"]["arcLengthHi"][0].GetDouble());
        bandWidthSummary.update(
                lineDoc["pathSummary"]["bandWidth"][0].GetDouble());
        
        // get time stamp of current frame:
        real timeStamp = lineDoc["pathSummary"]["timeStamp"][0].GetDouble();
        timeStamps.push_back(timeStamp);

        // get scalar time series data:
        argMinRadiusTimeSeries.push_back(lineDoc["pathSummary"]["argMinRadius"][0].GetDouble());
        minRadiusTimeSeries.push_back(lineDoc["pathSummary"]["minRadius"][0].GetDouble());
        lengthTimeSeries.push_back(lineDoc["pathSummary"]["length"][0].GetDouble());
        volumeTimeSeries.push_back(lineDoc["pathSummary"]["volume"][0].GetDouble());
        numPathwayTimeSeries.push_back(lineDoc["pathSummary"]["numPath"][0].GetDouble());
        numSampleTimeSeries.push_back(lineDoc["pathSummary"]["numSample"][0].GetDouble());
        argMinSolventDensityTimeSeries.push_back(lineDoc["pathSummary"]["argMinSolventDensity"][0].GetDouble());
        minSolventDensityTimeSeries.push_back(lineDoc["pathSummary"]["minSolventDensity"][0].GetDouble());
        bandWidthTimeSeries.push_back(lineDoc["pathSummary"]["bandWidth"][0].GetDouble());

        // in first line, also read number of residues in pore forming group:
        if( linesRead == 0 )
        {
            numPoreRes = lineDoc["residuePositions"]["resId"].Size();

            for(size_t i = 0; i < numPoreRes; i++)
            {
                poreResIds.push_back(
                        lineDoc["residuePositions"]["resId"][i].GetDouble());
            }
        }

        // increment line counter:
        linesRead++;
    }

    // close per frame data set:
    inFile.close();
    
    // sanity check:
    if( linesRead != numFrames )
    {
        throw std::runtime_error("Number of frames read does not equal number"
        "of frames analyised.");
    }


    // READ PER-FRAME DATA AND AGGREGATE TIME-AVERAGED PORE PROFILE
    // ------------------------------------------------------------------------

    // define set of support points for profile evaluation:
    std::vector<real> supportPoints;
    size_t numSupportPoints = outputNumPoints_;
    real supportPointsLo = arcLengthLoSummary.min() - outputExtrapDist_;
    real supportPointsHi = arcLengthHiSummary.max() + outputExtrapDist_;

    // build support points:
    real supportPointsStep = (supportPointsHi - supportPointsLo) / (numSupportPoints - 1);
    for(size_t i = 0; i < numSupportPoints; i++)
    {
        supportPoints.push_back(supportPointsLo + i*supportPointsStep);
    }

    // define anchor points at which energy is set to zero:
    real anchorPointLo = arcLengthLoSummary.min();
    real anchorPointHi = arcLengthHiSummary.max();
    SummaryStatistics anchorEnergyLo;
    SummaryStatistics anchorEnergyHi;

    // open JSON data file in read mode:
    inFile.open(inFileName.c_str(), std::fstream::in);
    
    // prepare containers for profile summaries:
    std::vector<SummaryStatistics> radiusSummary(supportPoints.size());
    std::vector<SummaryStatistics> solventDensitySummary(supportPoints.size());
    std::vector<SummaryStatistics> energySummary(supportPoints.size());
    std::vector<SummaryStatistics> plHydrophobicitySummary(supportPoints.size());
    std::vector<SummaryStatistics> pfHydrophobicitySummary(supportPoints.size());

    // prepare summary statistics for residue properties:
    std::vector<SummaryStatistics> residueArcSummary(numPoreRes);
    std::vector<SummaryStatistics> residueRhoSummary(numPoreRes);
    std::vector<SummaryStatistics> residuePhiSummary(numPoreRes);
    std::vector<SummaryStatistics> residuePlSummary(numPoreRes);
    std::vector<SummaryStatistics> residuePfSummary(numPoreRes);
    std::vector<SummaryStatistics> residuePoreRadiusSummary(numPoreRes);
    std::vector<SummaryStatistics> residueSolventDensitySummary(numPoreRes);
    std::vector<SummaryStatistics> residueXSummary(numPoreRes);
    std::vector<SummaryStatistics> residueYSummary(numPoreRes);
    std::vector<SummaryStatistics> residueZSummary(numPoreRes);

    // containers for profile valued time series: 
    std::vector<std::vector<real>> radiusProfileTimeSeries;
    std::vector<std::vector<real>> solventDensityTimeSeries;
    std::vector<std::vector<real>> plHydrophobicityTimeSeries;
    std::vector<std::vector<real>> pfHydrophobicityTimeSeries;

    // read file line by line:
    int linesProcessed = 0;
    while( std::getline(inFile, line) )
    {
        std::cout.precision(3);
        std::cout<<"\rForming time averages, "
                 <<(double)linesProcessed/numFrames*100
                 <<"\% complete"
                 <<std::flush;

        // read line into JSON document:
        rapidjson::StringStream lineStream(line.c_str());
        rapidjson::Document lineDoc;
        lineDoc.ParseStream(lineStream);

        // sanity checks:
        if( !lineDoc.IsObject() )
        {
            std::string error = "Line " + std::to_string(linesProcessed) + 
            " read from" + inFileName + " is not valid JSON object.";
            throw std::runtime_error(error);
        }

        // create molecular path:
        MolecularPath molPath(lineDoc);

        // sample radius at support points and add to summary statistics:
        std::vector<real> radiusSample = molPath.sampleRadii(supportPoints); 
        for(size_t i = 0; i < radiusSample.size(); i++)
        {
            radiusSummary.at(i).update(radiusSample.at(i));
        }

        // add to time series:
        radiusProfileTimeSeries.push_back(radiusSample);

        
        // sample points from hydrophobicity splines:
        SplineCurve1D pfHydrophobicitySpline = SplineCurve1DJsonConverter::fromJson(
                lineDoc["pfHydrophobicitySpline"], 1);
        std::vector<real> pfHydrophobicitySample = 
                pfHydrophobicitySpline.evaluateMultiple(supportPoints, 0);
        SummaryStatistics::updateMultiple(
                pfHydrophobicitySummary,
                pfHydrophobicitySample);
        pfHydrophobicityTimeSeries.push_back(pfHydrophobicitySample);

        SplineCurve1D plHydrophobicitySpline = SplineCurve1DJsonConverter::fromJson(
                lineDoc["plHydrophobicitySpline"], 1);
        std::vector<real> plHydrophobicitySample = 
                plHydrophobicitySpline.evaluateMultiple(supportPoints, 0);
        SummaryStatistics::updateMultiple(
                plHydrophobicitySummary,
                plHydrophobicitySample);
        plHydrophobicityTimeSeries.push_back(plHydrophobicitySample);


        // sample points from solvent density spline:
        SplineCurve1D solventDensitySpline = SplineCurve1DJsonConverter::fromJson(
                lineDoc["solventDensitySpline"], 1);
        std::vector<real> solventDensitySample = 
                solventDensitySpline.evaluateMultiple(supportPoints, 0);

        // get total number of particles in sample for this time step:
        int totalNumber = lineDoc["pathSummary"]["numSample"][0].GetDouble();

        // convert to number density and add to summary statistic:
        // TODO this should be done in per-frame analysis:
        NumberDensityCalculator ndc;
        solventDensitySample = ndc(
                solventDensitySample, 
                radiusSample, 
                totalNumber);
        SummaryStatistics::updateMultiple(
                solventDensitySummary,
                solventDensitySample);
        solventDensityTimeSeries.push_back(solventDensitySample);
 
        // convert to energy and add to summary statistic:
        BoltzmannEnergyCalculator bec;
        std::vector<real> energySample = bec.calculate(solventDensitySample);
        SummaryStatistics::updateMultiple(
                energySummary,
                energySample);

        // also evaluate density and radius at anchor points:
        real solventDensityAnchorLo = solventDensitySpline.evaluate(
                anchorPointLo, 0);
        real solventDensityAnchorHi = solventDensitySpline.evaluate(
                anchorPointHi, 0);
        real poreRadiusAnchorLo = molPath.radius(anchorPointLo);
        real poreRadiusAnchorHi = molPath.radius(anchorPointHi);

        // calculate energy at anchor points by linear interpolation:
        LinearSplineInterp1D interp;
        auto energySpline = interp(supportPoints, energySample);
        anchorEnergyLo.update( energySpline.evaluate(anchorPointLo, 0) );
        anchorEnergyHi.update( energySpline.evaluate(anchorPointHi, 0) );


        // loop over all pore forming residues:
        for(size_t i = 0; i < numPoreRes; i++)
        {
            residueArcSummary.at(i).update(
                    lineDoc["residuePositions"]["s"][i].GetDouble());
            residueRhoSummary.at(i).update(
                    lineDoc["residuePositions"]["rho"][i].GetDouble());
            residuePhiSummary.at(i).update(
                    lineDoc["residuePositions"]["phi"][i].GetDouble());
            residuePlSummary.at(i).update(
                    lineDoc["residuePositions"]["poreLining"][i].GetDouble());
            residuePfSummary.at(i).update(
                    lineDoc["residuePositions"]["poreFacing"][i].GetDouble());
            residueXSummary.at(i).update(
                    lineDoc["residuePositions"]["x"][i].GetDouble());
            residueYSummary.at(i).update(
                    lineDoc["residuePositions"]["y"][i].GetDouble());
            residueZSummary.at(i).update(
                    lineDoc["residuePositions"]["z"][i].GetDouble());

            // residue-local number density requires additional post-processing:
            real rad = lineDoc["residuePositions"]["poreRadius"][i].GetDouble();
            real den = lineDoc["residuePositions"]["solventDensity"][i].GetDouble();
            residuePoreRadiusSummary.at(i).update(rad);
            residueSolventDensitySummary.at(i).update(den*totalNumber/(M_PI*rad*rad));
        }

        // increment line counter:
        linesProcessed++;
    }
  
    // shift of energy profile so that energy at anchor points is zero:
    real shift = -0.5*(anchorEnergyLo.mean() + anchorEnergyHi.mean());
    std::for_each(
            energySummary.begin(), 
            energySummary.end(), 
            [this, shift](SummaryStatistics &s){s.shift(shift);});

    // inform user about progress:
    std::cout.precision(3);
    std::cout<<"\rForming time averages, "
             <<(double)linesProcessed/numFrames*100
             <<"\% complete"
             <<std::endl;

    // sanity check:
    if( linesProcessed != numFrames )
    {
        std::string error = "Number of lines read from JSON file does not"
        "equal number of frames processed!"; 
        throw std::runtime_error(error);
    }

    // close filestream object:
    inFile.close();

    
    // CREATE PDB OUTPUT
    // ------------------------------------------------------------------------

    // assign residue pore facing and pore lining to occupency and bfac:
    outputStructure_.setPoreFacing(residuePlSummary, residuePfSummary);

    // write structure to PDB file:
    PdbIo::write(outputPdbFileName_, outputStructure_);


    // CREATE OUTPUT JSON
    // ------------------------------------------------------------------------

    // initialise a JSON results container:
    ResultsJsonExporter results;

    // add summary statistics for scalr variables describing the pathway:
    results.addPathwaySummary("argMinRadius", argMinRadiusSummary);
    results.addPathwaySummary("minRadius", minRadiusSummary);
    results.addPathwaySummary("length", lengthSummary);
    results.addPathwaySummary("volume", volumeSummary);
    results.addPathwaySummary("numPathway", numPathSummary);
    results.addPathwaySummary("numSample", numSampleSummary);
    results.addPathwaySummary("argMinSolventDensity", argMinSolventDensitySummary);
    results.addPathwaySummary("minSolventDensity", minSolventDensitySummary);
    results.addPathwaySummary("bandWidth", bandWidthSummary);

    // add time-averaged pathway profiles:
    results.addSupportPoints(supportPoints);
    results.addPathwayProfile("radius", radiusSummary);
    results.addPathwayProfile("plHydrophobicity", plHydrophobicitySummary);
    results.addPathwayProfile("pfHydrophobicity", pfHydrophobicitySummary);
    results.addPathwayProfile("density", solventDensitySummary);
    results.addPathwayProfile("energy", energySummary);
    
    // add scalar time series data to output:
    results.addTimeStamps(timeStamps);
    results.addPathwayScalarTimeSeries("argMinRadius", argMinRadiusTimeSeries);
    results.addPathwayScalarTimeSeries("minRadius", minRadiusTimeSeries);
    results.addPathwayScalarTimeSeries("length", lengthTimeSeries);
    results.addPathwayScalarTimeSeries("volume", volumeTimeSeries);
    results.addPathwayScalarTimeSeries("numPathway", numPathwayTimeSeries);
    results.addPathwayScalarTimeSeries("numSample", numSampleTimeSeries);
    results.addPathwayScalarTimeSeries("argMinSolventDensity", argMinSolventDensityTimeSeries);
    results.addPathwayScalarTimeSeries("minSolventDensity", minSolventDensityTimeSeries);
    results.addPathwayScalarTimeSeries("bandWidth", bandWidthTimeSeries);

    // add vector-valued time series data to output:
    results.addPathwayGridPoints(timeStamps, supportPoints);
    results.addPathwayProfileTimeSeries("radius", radiusProfileTimeSeries);
    results.addPathwayProfileTimeSeries("density", solventDensityTimeSeries);
    results.addPathwayProfileTimeSeries("plHydrophobicity", plHydrophobicityTimeSeries);
    results.addPathwayProfileTimeSeries("pfHydrophobicity", pfHydrophobicityTimeSeries);

    // add per-residue data to output document:
    results.addResidueInformation(poreResIds, resInfo_);
    results.addResidueSummary("s", residueArcSummary);
    results.addResidueSummary("rho", residueRhoSummary);
    results.addResidueSummary("phi", residuePhiSummary);
    results.addResidueSummary("poreLining", residuePlSummary);
    results.addResidueSummary("poreFacing", residuePfSummary);
    results.addResidueSummary("poreRadius", residuePoreRadiusSummary);
    results.addResidueSummary("solventDensity", residueSolventDensitySummary);
    results.addResidueSummary("x", residueXSummary);
    results.addResidueSummary("y", residueYSummary);
    results.addResidueSummary("z", residueZSummary);


    // write results to JSON file:
    results.write(outFileName);


    // COPYING PER-FRAME DATA TO FINAL OUTPUT FILE
    // ------------------------------------------------------------------------
    
    // open file with per-frame data and output data:
    inFile.open(inFileName, std::fstream::in);
    outFile.open(outFileName, std::fstream::app);

    // append input file to output file line by line:    
    int linesCopied = 0;
    std::string copyLine;
    while( std::getline(inFile, copyLine) )
    {
        // append line to out file:
        outFile<<copyLine<<std::endl;

        // increment line counter:
        linesCopied++;
    }

    // close file streams:
    inFile.close();
    outFile.close();

    // sanity checks:
    if( linesCopied != numFrames )
    {
        throw std::runtime_error("Could not copy all lines from per-frame data"
        "file to output data file.");
    }

    // delete temporary file:
    std::remove(inFileName.c_str());
}




void
trajectoryAnalysis::writeOutput()
{

}















