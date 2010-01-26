/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#ifdef OSGEARTH_HAVE_GEOS

#include <osgEarthFeatures/GEOS>
#include <osg/Notify>

#include <geos/geom/PrecisionModel.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/geom/Coordinate.h>
#include <geos/geom/CoordinateSequence.h>
#include <geos/geom/CoordinateArraySequenceFactory.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/Point.h>
#include <geos/geom/MultiPoint.h>
#include <geos/geom/Polygon.h>
#include <geos/geom/MultiPolygon.h>
#include <geos/geom/LineString.h>
#include <geos/geom/MultiLineString.h>
#include <geos/geom/LinearRing.h>
#include <geos/operation/valid/IsValidOp.h>
#include <geos/util/IllegalArgumentException.h>

using namespace osgEarth;
using namespace osgEarth::Features;
using namespace geos;
using namespace geos::operation;


static
geom::CoordinateSequence*
vec3dArray2CoordSeq( const osg::Vec3dArray* input, bool close, const geom::CoordinateSequenceFactory* factory )
{   
    bool needToClose = close && input->size() > 2 && input->front() != input->back();

    std::vector<geos::geom::Coordinate>* coords = new std::vector<geom::Coordinate>();
    coords->reserve( input->size() + (needToClose ? 1 : 0) );
    for( osg::Vec3dArray::const_iterator i = input->begin(); i != input->end(); ++i )
    {
        coords->push_back( geom::Coordinate( i->x(), i->y() ) ); //, i->z() ));
    }
    if ( needToClose )
    {
        coords->push_back( coords->front() );
    }
    geom::CoordinateSequence* seq = factory->create( coords );

    return seq;
}

static geom::Geometry*
import( const Geometry* input, const geom::GeometryFactory* f )
{
    geom::Geometry* output = 0L;

    if ( input->getType() == Geometry::TYPE_UNKNOWN )
    {
        output = 0L;
    }
    else if ( input->getType() == Geometry::TYPE_MULTI )
    {
        const MultiGeometry* multi = static_cast<const MultiGeometry*>( input );

        Geometry::Type compType = multi->getComponentType();

        std::vector<geom::Geometry*>* children = new std::vector<geom::Geometry*>();
        for( GeometryCollection::const_iterator i = multi->getComponents().begin(); i != multi->getComponents().end(); ++i ) 
        {
            geom::Geometry* child = import( i->get(), f );
            if ( child )
                children->push_back( child );
        }
        if ( children->size() > 0 )
        {
            if ( compType == Geometry::TYPE_POLYGON )
                output = f->createMultiPolygon( children );
            else if ( compType == Geometry::TYPE_LINESTRING )
                output = f->createMultiLineString( children );
            else if ( compType == Geometry::TYPE_POINTSET )
                output = f->createMultiPoint( children );
            else
                output = f->createGeometryCollection( children );
        }
        else
            delete children;
    }
    else
    {
        // any other type will at least contain points:
        geom::CoordinateSequence* seq = 0L;
        try
        {
            switch( input->getType() )
            {
            case Geometry::TYPE_POINTSET:
                seq = vec3dArray2CoordSeq( input, false, f->getCoordinateSequenceFactory() );
                if ( seq ) output = f->createPoint( seq );
                break;

            case Geometry::TYPE_LINESTRING:
                seq = vec3dArray2CoordSeq( input, false, f->getCoordinateSequenceFactory() );
                if ( seq ) output = f->createLineString( seq );
                break;

            case Geometry::TYPE_RING:
                seq = vec3dArray2CoordSeq( input, true, f->getCoordinateSequenceFactory() );
                if ( seq ) output = f->createLinearRing( seq );
                break;

            case Geometry::TYPE_POLYGON:
                const Polygon* poly = static_cast<const Polygon*>(input);
                seq = vec3dArray2CoordSeq( input, true, f->getCoordinateSequenceFactory() );
                geom::LinearRing* shell = 0L;
                if ( seq )
                    shell = f->createLinearRing( seq );

                if ( shell )
                {
                    const Polygon* poly = static_cast<const Polygon*>(input);
                    std::vector<geom::Geometry*>* holes = poly->getHoles().size() > 0 ? new std::vector<geom::Geometry*>() : 0L;
                    for( RingCollection::const_iterator r = poly->getHoles().begin(); r != poly->getHoles().end(); ++r )
                    {
                        geom::Geometry* hole = import( r->get(), f );
                        if ( hole ) holes->push_back( hole );
                    }
                    if ( holes && holes->size() == 0 )
                    {
                        delete holes;
                        holes = 0L;
                    }
                    output = f->createPolygon( shell, holes );
                }
                
                break;
            }
        }
        catch( util::IllegalArgumentException )
        {
            // catch GEOS exceptions..
            //if ( seq )
            //    delete seq;

            osg::notify(osg::NOTICE) << "[osgEarth] GEOS::import: Removed degenerate geometry" << std::endl;
        }
    }

    return output;
}

geom::Geometry*
GEOSUtils::importGeometry( const Geometry* input )
{
    geom::Geometry* output = 0L;
    if ( input && input->isValid() )
    {
        geom::PrecisionModel* pm = new geom::PrecisionModel( geom::PrecisionModel::FLOATING );
        const geom::GeometryFactory* f = new geom::GeometryFactory( pm );

        output = import( input, f );

        // if output is ok, it will have a pointer to f. this is probably a leak.
        if ( !output )
            delete f;
    }
    return output;
}

static Geometry*
exportPolygon( const geom::Polygon* input )
{
    Polygon* output = 0L;
    const geom::LineString* outerRing = input->getExteriorRing();
    if ( outerRing )
    {
        const geom::CoordinateSequence* s = outerRing->getCoordinates();
        output = new Polygon( s->getSize() );
        for( int j=0; j<s->getSize(); j++ ) 
        {
            const geom::Coordinate& c = s->getAt( j );
            output->push_back( osg::Vec3d( c.x, c.y, 0 ) ); 
        }
        output->rewind( Ring::ORIENTATION_CCW );

        for( int k=0; k < input->getNumInteriorRing(); k++ )
        {
            const geom::LineString* inner = input->getInteriorRingN( k );
            const geom::CoordinateSequence* s = inner->getCoordinates();
            Ring* hole = new Ring( s->getSize() );
            for( int m = 0; m<s->getSize(); m++ )
            {
                const geom::Coordinate& c = s->getAt( m );
                hole->push_back( osg::Vec3d( c.x, c.y, 0 ) );
            }
            hole->rewind( Ring::ORIENTATION_CW );
            output->getHoles().push_back( hole );
        }
    }
    return output;
}

Geometry*
GEOSUtils::exportGeometry( const geom::Geometry* input )
{
    //// first verify that the input is valid.
    //valid::IsValidOp validator( input );
    //if ( !validator.isValid() )
    //{
    //    osg::notify(osg::NOTICE) << "[osgEarth] GEOS: discarding invalid geometry" << std::endl;
    //    return 0L;
    //}

    GeometryCollection parts;

    if ( dynamic_cast<const geom::Point*>( input ) )
    {
        osg::notify(osg::NOTICE) << "[osgEarth] GEOS 'Point' NYI" << std::endl;        
    }
    else if ( dynamic_cast<const geom::MultiPoint*>( input ) )
    {
        const geom::MultiPoint* mp = static_cast<const geom::MultiPoint*>( input );
        PointSet* part = new PointSet( mp->getNumPoints() );
        for( int i=0; i < mp->getNumPoints(); i++ )
        {
            geom::Point* p = (geom::Point*)( mp->getGeometryN(i) );
            part->push_back( osg::Vec3d( p->getX(), p->getY(), 0 ) );
        }
        parts.push_back( part );
    }
    else if ( dynamic_cast<const geom::LineString*>( input ) )
    {
        const geom::LineString* line = static_cast<const geom::LineString*>( input );
        LineString* part = new LineString( line->getNumPoints() );
        for( int i=0; i<line->getNumPoints(); i++ )
        {
            const geom::Coordinate& c = line->getCoordinateN(i);
            part->push_back( osg::Vec3d( c.x, c.y, 0 ) );
        }
        parts.push_back( part );
    }
    else if ( dynamic_cast<const geom::MultiLineString*>( input ) )
    {
        const geom::MultiLineString* m = static_cast<const geom::MultiLineString*>( input );
        for( int i=0; i<m->getNumGeometries(); i++ ) 
        {
            Geometry* part = exportGeometry( m->getGeometryN(i) );
            if ( part ) parts.push_back( part );
        }
    }
    else if ( dynamic_cast<const geom::Polygon*>( input ) )
    {
        const geom::Polygon* poly = static_cast<const geom::Polygon*>( input );
        Geometry* part = exportPolygon( poly );
        if ( part ) parts.push_back( part );
    }
    else if ( dynamic_cast<const geom::MultiPolygon*>( input ) )
    {
        //osg::notify(osg::NOTICE) << "Multipolygon" << std::endl;
        const geom::MultiPolygon* mpoly = static_cast<const geom::MultiPolygon*>( input );
        for( int i=0; i<mpoly->getNumGeometries(); i++ )
        {
            Geometry* part = exportPolygon( static_cast<const geom::Polygon*>( mpoly->getGeometryN(i) ) );
            if ( part ) parts.push_back( part );
        }        
    }

    if ( parts.size() == 1 )
    {
        osg::ref_ptr<Geometry> part = parts.front();
        parts.clear();
        return part.release();
    }
    else if ( parts.size() > 1 )
    {
        return new MultiGeometry( parts );
    }
    else
    {
        return 0L;
    }
}

#endif // OSGEARTH_HAVE_GEOS
