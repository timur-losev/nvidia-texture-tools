// Copyright NVIDIA Corporation 2007 -- Ignacio Castano <icastano@nvidia.com>
// 
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include <nvmath/Color.h>

#include <nvimage/ColorBlock.h>
#include <nvimage/BlockDXT.h>

#include "QuickCompressDXT.h"
#include "SingleColorLookup.h"


using namespace nv;
using namespace QuickCompress;



inline static void extractColorBlockRGB(const ColorBlock & rgba, Vector3 block[16])
{
	for (int i = 0; i < 16; i++)
	{
		const Color32 c = rgba.color(i);
		block[i] = Vector3(c.r, c.g, c.b);
	}
}

inline static uint extractColorBlockRGBA(const ColorBlock & rgba, Vector3 block[16])
{
	int num = 0;
	
	for (int i = 0; i < 16; i++)
	{
		const Color32 c = rgba.color(i);
		if (c.a > 127)
		{
			block[num++] = Vector3(c.r, c.g, c.b);
		}
	}
	
	return num;
}


// find minimum and maximum colors based on bounding box in color space
inline static void findMinMaxColorsBox(const Vector3 * block, uint num, Vector3 * restrict maxColor, Vector3 * restrict minColor)
{
	*maxColor = Vector3(0, 0, 0);
	*minColor = Vector3(255, 255, 255);
	
	for (uint i = 0; i < num; i++)
	{
		*maxColor = max(*maxColor, block[i]);
		*minColor = min(*minColor, block[i]);
	}
}


inline static void selectDiagonal(const Vector3 * block, uint num, Vector3 * restrict maxColor, Vector3 * restrict minColor)
{
	Vector3 center = (*maxColor + *minColor) * 0.5;

	Vector2 covariance = Vector2(zero);
	for (uint i = 0; i < num; i++)
	{
		Vector3 t = block[i] - center;
		covariance += t.xy() * t.z();
	}

	float x0 = maxColor->x();
	float y0 = maxColor->y();
	float x1 = minColor->x();
	float y1 = minColor->y();
	
	if (covariance.x() < 0) {
		swap(x0, x1);
	}
	if (covariance.y() < 0) {
		swap(y0, y1);
	}
	
	maxColor->set(x0, y0, maxColor->z());
	minColor->set(x1, y1, minColor->z());
}

inline static void insetBBox(Vector3 * restrict maxColor, Vector3 * restrict minColor)
{
	Vector3 inset = (*maxColor - *minColor) / 16.0f - (8.0f / 255.0f) / 16.0f;
	*maxColor = clamp(*maxColor - inset, 0.0f, 255.0f);
	*minColor = clamp(*minColor + inset, 0.0f, 255.0f);
}

inline static uint16 roundAndExpand(Vector3 * restrict v)
{
	uint r = uint(clamp(v->x() * (31.0f / 255.0f), 0.0f, 31.0f) + 0.5f);
	uint g = uint(clamp(v->y() * (63.0f / 255.0f), 0.0f, 63.0f) + 0.5f);
	uint b = uint(clamp(v->z() * (31.0f / 255.0f), 0.0f, 31.0f) + 0.5f);
	
	uint16 w = (r << 11) | (g << 5) | b;

	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	*v = Vector3(float(r), float(g), float(b));
	
	return w;
}

inline static float colorDistance(Vector3::Arg c0, Vector3::Arg c1)
{
	return dot(c0-c1, c0-c1);
}

inline static uint computeIndices4(Vector3 block[16], Vector3::Arg maxColor, Vector3::Arg minColor)
{
	Vector3 palette[4];
	palette[0] = maxColor;
	palette[1] = minColor;
	palette[2] = lerp(palette[0], palette[1], 1.0f / 3.0f);
	palette[3] = lerp(palette[0], palette[1], 2.0f / 3.0f);
	
	uint indices = 0;
	for(int i = 0; i < 16; i++)
	{
		float d0 = colorDistance(palette[0], block[i]);
		float d1 = colorDistance(palette[1], block[i]);
		float d2 = colorDistance(palette[2], block[i]);
		float d3 = colorDistance(palette[3], block[i]);
		
		uint b0 = d0 > d3;
		uint b1 = d1 > d2;
		uint b2 = d0 > d2;
		uint b3 = d1 > d3;
		uint b4 = d2 > d3;
		
		uint x0 = b1 & b2;
		uint x1 = b0 & b3;
		uint x2 = b0 & b4;
		
		indices |= (x2 | ((x0 | x1) << 1)) << (2 * i);
	}

	return indices;
}

inline static uint computeIndices3(const ColorBlock & rgba, Vector3::Arg maxColor, Vector3::Arg minColor)
{
	Vector3 palette[4];
	palette[0] = minColor;
	palette[1] = maxColor;
	palette[2] = (palette[0] + palette[1]) * 0.5f;
	
	uint indices = 0;
	for(int i = 0; i < 16; i++)
	{
		Color32 c = rgba.color(i);
		Vector3 color = Vector3(c.r, c.g, c.b);
		
		float d0 = colorDistance(palette[0], color);
		float d1 = colorDistance(palette[1], color);
		float d2 = colorDistance(palette[2], color);
		
		uint index;
		if (c.a < 128) index = 3;
		else if (d0 < d1 && d0 < d2) index = 0;
		else if (d1 < d2) index = 1;
		else index = 2;
		
		indices |= index << (2 * i);
	}

	return indices;
}


static void optimizeEndPoints4(Vector3 block[16], BlockDXT1 * dxtBlock)
{
	float alpha2_sum = 0.0f;
	float beta2_sum = 0.0f;
	float alphabeta_sum = 0.0f;
	Vector3 alphax_sum(zero);
	Vector3 betax_sum(zero);
	
	for( int i = 0; i < 16; ++i )
	{
		const uint bits = dxtBlock->indices >> (2 * i);
		
		float beta = float(bits & 1);
		if (bits & 2) beta = (1 + beta) / 3.0f;
		float alpha = 1.0f - beta;
		
		alpha2_sum += alpha * alpha;
		beta2_sum += beta * beta;
		alphabeta_sum += alpha * beta;
		alphax_sum += alpha * block[i];
		betax_sum += beta * block[i];
	}

	float denom = alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum;
	if (equal(denom, 0.0f)) return;
	
	float factor = 1.0f / denom;
	
	Vector3 a = (alphax_sum * beta2_sum - betax_sum * alphabeta_sum) * factor;
	Vector3 b = (betax_sum * alpha2_sum - alphax_sum * alphabeta_sum) * factor;

	a = clamp(a, 0, 255);
	b = clamp(b, 0, 255);
	
	uint16 color0 = roundAndExpand(&a);
	uint16 color1 = roundAndExpand(&b);

	if (color0 < color1)
	{
		swap(a, b);
		swap(color0, color1);
	}

	dxtBlock->col0 = Color16(color0);
	dxtBlock->col1 = Color16(color1);
	dxtBlock->indices = computeIndices4(block, a, b);
}

/*static void optimizeEndPoints3(Vector3 block[16], BlockDXT1 * dxtBlock)
{
	float alpha2_sum = 0.0f;
	float beta2_sum = 0.0f;
	float alphabeta_sum = 0.0f;
	Vector3 alphax_sum(zero);
	Vector3 betax_sum(zero);
	
	for( int i = 0; i < 16; ++i )
	{
		const uint bits = dxtBlock->indices >> (2 * i);

		float beta = (bits & 1);
		if (bits & 2) beta = 0.5f;
		float alpha = 1.0f - beta;

		alpha2_sum += alpha * alpha;
		beta2_sum += beta * beta;
		alphabeta_sum += alpha * beta;
		alphax_sum += alpha * block[i];
		betax_sum += beta * block[i];
	}

	float denom = alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum;
	if (equal(denom, 0.0f)) return;
	
	float factor = 1.0f / denom;
	
	Vector3 a = (alphax_sum * beta2_sum - betax_sum * alphabeta_sum) * factor;
	Vector3 b = (betax_sum * alpha2_sum - alphax_sum * alphabeta_sum) * factor;

	a = clamp(a, 0, 255);
	b = clamp(b, 0, 255);
	
	uint16 color0 = roundAndExpand(&a);
	uint16 color1 = roundAndExpand(&b);

	if (color0 < color1)
	{
		swap(a, b);
		swap(color0, color1);
	}

	dxtBlock->col0 = Color16(color1);
	dxtBlock->col1 = Color16(color0);
	dxtBlock->indices = computeIndices3(block, a, b);
}*/

namespace
{
	static int computeGreenError(const ColorBlock & rgba, const BlockDXT1 * block)
	{
		nvDebugCheck(block != NULL);

		int palette[4];
		palette[0] = (block->col0.g << 2) | (block->col0.g >> 4);
		palette[1] = (block->col1.g << 2) | (block->col1.g >> 4);
		palette[2] = (2 * palette[0] + palette[1]) / 3;
		palette[3] = (2 * palette[1] + palette[0]) / 3;

		int totalError = 0;

		for (int i = 0; i < 16; i++)
		{
			const int green = rgba.color(i).g;
			
			int error = abs(green - palette[0]);
			error = min(error, abs(green - palette[1]));
			error = min(error, abs(green - palette[2]));
			error = min(error, abs(green - palette[3]));
			
			totalError += error;
		}

		return totalError;
	}

	static uint computeGreenIndices(const ColorBlock & rgba, const Color32 palette[4])
	{
		const int color0 = palette[0].g;
		const int color1 = palette[1].g;
		const int color2 = palette[2].g;
		const int color3 = palette[3].g;
		
		uint indices = 0;
		for (int i = 0; i < 16; i++)
		{
			const int color = rgba.color(i).g;
			
			uint d0 = abs(color0 - color);
			uint d1 = abs(color1 - color);
			uint d2 = abs(color2 - color);
			uint d3 = abs(color3 - color);
			
			uint b0 = d0 > d3;
			uint b1 = d1 > d2;
			uint b2 = d0 > d2;
			uint b3 = d1 > d3;
			uint b4 = d2 > d3;
			
			uint x0 = b1 & b2;
			uint x1 = b0 & b3;
			uint x2 = b0 & b4;
			
			indices |= (x2 | ((x0 | x1) << 1)) << (2 * i);
		}

		return indices;
	}

} // namespace

namespace
{

	static uint computeAlphaIndices(const ColorBlock & rgba, AlphaBlockDXT5 * block)
	{
		uint8 alphas[8];
		block->evaluatePalette(alphas);

		uint totalError = 0;

		for (uint i = 0; i < 16; i++)
		{
			uint8 alpha = rgba.color(i).a;

			uint besterror = 256*256;
			uint best = 8;
			for(uint p = 0; p < 8; p++)
			{
				int d = alphas[p] - alpha;
				uint error = d * d;

				if (error < besterror)
				{
					besterror = error;
					best = p;
				}
			}
			nvDebugCheck(best < 8);

			totalError += besterror;
			block->setIndex(i, best);
		}

		return totalError;
	}

	static void optimizeAlpha8(const ColorBlock & rgba, AlphaBlockDXT5 * block)
	{
		float alpha2_sum = 0;
		float beta2_sum = 0;
		float alphabeta_sum = 0;
		float alphax_sum = 0;
		float betax_sum = 0;

		for (int i = 0; i < 16; i++)
		{
			uint idx = block->index(i);
			float alpha;
			if (idx < 2) alpha = 1.0f - idx;
			else alpha = (8.0f - idx) / 7.0f;
			
			float beta = 1 - alpha;

			alpha2_sum += alpha * alpha;
			beta2_sum += beta * beta;
			alphabeta_sum += alpha * beta;
			alphax_sum += alpha * rgba.color(i).a;
			betax_sum += beta * rgba.color(i).a;
		}

		const float factor = 1.0f / (alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum);

		float a = (alphax_sum * beta2_sum - betax_sum * alphabeta_sum) * factor;
		float b = (betax_sum * alpha2_sum - alphax_sum * alphabeta_sum) * factor;

		uint alpha0 = uint(min(max(a, 0.0f), 255.0f));
		uint alpha1 = uint(min(max(b, 0.0f), 255.0f));

		if (alpha0 < alpha1)
		{
			swap(alpha0, alpha1);

			// Flip indices:
			for (int i = 0; i < 16; i++)
			{
				uint idx = block->index(i);
				if (idx < 2) block->setIndex(i, 1 - idx);
				else block->setIndex(i, 9 - idx);
			}
		}
		else if (alpha0 == alpha1)
		{
			for (int i = 0; i < 16; i++)
			{
				block->setIndex(i, 0);
			}
		}

		block->alpha0 = alpha0;
		block->alpha1 = alpha1;
	}

	/*
	static void optimizeAlpha6(const ColorBlock & rgba, AlphaBlockDXT5 * block)
	{
		float alpha2_sum = 0;
		float beta2_sum = 0;
		float alphabeta_sum = 0;
		float alphax_sum = 0;
		float betax_sum = 0;

		for (int i = 0; i < 16; i++)
		{
			uint8 x = rgba.color(i).a;
			if (x == 0 || x == 255) continue;

			uint bits = block->index(i);
			if (bits == 6 || bits == 7) continue;

			float alpha;
			if (bits == 0) alpha = 1.0f;
			else if (bits == 1) alpha = 0.0f;
			else alpha = (6.0f - block->index(i)) / 5.0f;
			
			float beta = 1 - alpha;

			alpha2_sum += alpha * alpha;
			beta2_sum += beta * beta;
			alphabeta_sum += alpha * beta;
			alphax_sum += alpha * x;
			betax_sum += beta * x;
		}

		const float factor = 1.0f / (alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum);

		float a = (alphax_sum * beta2_sum - betax_sum * alphabeta_sum) * factor;
		float b = (betax_sum * alpha2_sum - alphax_sum * alphabeta_sum) * factor;

		uint alpha0 = uint(min(max(a, 0.0f), 255.0f));
		uint alpha1 = uint(min(max(b, 0.0f), 255.0f));

		if (alpha0 > alpha1)
		{
			swap(alpha0, alpha1);
		}

		block->alpha0 = alpha0;
		block->alpha1 = alpha1;
	}
	*/

	static bool sameIndices(const AlphaBlockDXT5 & block0, const AlphaBlockDXT5 & block1)
	{
		const uint64 mask = ~uint64(0xFFFF);
		return (block0.u | mask) == (block1.u | mask);
	}

} // namespace





// Single color compressor, based on:
// https://mollyrocket.com/forums/viewtopic.php?t=392
void QuickCompress::compressDXT1(Color32 c, BlockDXT1 * dxtBlock)
{
	dxtBlock->col0.r = OMatch5[c.r][0];
	dxtBlock->col0.g = OMatch6[c.g][0];
	dxtBlock->col0.b = OMatch5[c.b][0];
	dxtBlock->col1.r = OMatch5[c.r][1];
	dxtBlock->col1.g = OMatch6[c.g][1];
	dxtBlock->col1.b = OMatch5[c.b][1];
	dxtBlock->indices = 0xaaaaaaaa;

	if (dxtBlock->col0.u < dxtBlock->col1.u)
	{
		swap(dxtBlock->col0.u, dxtBlock->col1.u);
		dxtBlock->indices ^= 0x55555555;
	}
}

void QuickCompress::compressDXT1(const ColorBlock & rgba, BlockDXT1 * dxtBlock)
{
	// read block
	Vector3 block[16];
	extractColorBlockRGB(rgba, block);
	
	// find min and max colors
	Vector3 maxColor, minColor;
	findMinMaxColorsBox(block, 16, &maxColor, &minColor);
	
	selectDiagonal(block, 16, &maxColor, &minColor);
	
	insetBBox(&maxColor, &minColor);
	
	uint16 color0 = roundAndExpand(&maxColor);
	uint16 color1 = roundAndExpand(&minColor);

	if (color0 < color1)
	{
		swap(maxColor, minColor);
		swap(color0, color1);
	}

	dxtBlock->col0 = Color16(color0);
	dxtBlock->col1 = Color16(color1);
	dxtBlock->indices = computeIndices4(block, maxColor, minColor);

	optimizeEndPoints4(block, dxtBlock);
}


void QuickCompress::compressDXT1a(Color32 rgba, BlockDXT1 * dxtBlock)
{
	if (rgba.a == 0)
	{
		dxtBlock->col0.u = 0;
		dxtBlock->col1.u = 0;
		dxtBlock->indices = 0xFFFFFFFF;
	}
	else
	{
		compressDXT1(rgba, dxtBlock);
	}
}

void QuickCompress::compressDXT1a(const ColorBlock & rgba, BlockDXT1 * dxtBlock)
{
	if (!rgba.hasAlpha())
	{
		compressDXT1(rgba, dxtBlock);
	}
	else
	{
		// read block
		Vector3 block[16];
		uint num = extractColorBlockRGBA(rgba, block);
		
		// find min and max colors
		Vector3 maxColor, minColor;
		findMinMaxColorsBox(block, num, &maxColor, &minColor);
		
		selectDiagonal(block, num, &maxColor, &minColor);
		
		insetBBox(&maxColor, &minColor);
		
		uint16 color0 = roundAndExpand(&maxColor);
		uint16 color1 = roundAndExpand(&minColor);
		
		if (color0 < color1)
		{
			swap(maxColor, minColor);
			swap(color0, color1);
		}
		
		dxtBlock->col0 = Color16(color1);
		dxtBlock->col1 = Color16(color0);
		dxtBlock->indices = computeIndices3(rgba, maxColor, minColor);
		
		//	optimizeEndPoints(block, dxtBlock);
	}
}


// Brute force green channel compressor
void QuickCompress::compressDXT1G(const ColorBlock & rgba, BlockDXT1 * block)
{
	nvDebugCheck(block != NULL);
	
	uint8 ming = 63;
	uint8 maxg = 0;
	
	// Get min/max green.
	for (uint i = 0; i < 16; i++)
	{
		uint8 green = rgba.color(i).g >> 2;
		ming = min(ming, green);
		maxg = max(maxg, green);
	}

	block->col0.r = 31;
	block->col1.r = 31;
	block->col0.g = maxg;
	block->col1.g = ming;
	block->col0.b = 0;
	block->col1.b = 0;

	if (maxg - ming > 4)
	{
		int besterror = computeGreenError(rgba, block);
		int bestg0 = maxg;
		int bestg1 = ming;
		
		for (int g0 = ming+5; g0 < maxg; g0++)
		{
			for (int g1 = ming; g1 < g0-4; g1++)
			{
				if ((maxg-g0) + (g1-ming) > besterror)
					continue;
				
				block->col0.g = g0;
				block->col1.g = g1;
				int error = computeGreenError(rgba, block);
				
				if (error < besterror)
				{
					besterror = error;
					bestg0 = g0;
					bestg1 = g1;
				}
			}
		}
		
		block->col0.g = bestg0;
		block->col1.g = bestg1;
	}
	
	Color32 palette[4];
	block->evaluatePalette(palette);
	block->indices = computeGreenIndices(rgba, palette);
}



void QuickCompress::compressDXT3A(const ColorBlock & rgba, AlphaBlockDXT3 * dxtBlock)
{
	// @@ Round instead of truncate. When rounding take into account bit expansion.
	dxtBlock->alpha0 = rgba.color(0).a >> 4;
	dxtBlock->alpha1 = rgba.color(1).a >> 4;
	dxtBlock->alpha2 = rgba.color(2).a >> 4;
	dxtBlock->alpha3 = rgba.color(3).a >> 4;
	dxtBlock->alpha4 = rgba.color(4).a >> 4;
	dxtBlock->alpha5 = rgba.color(5).a >> 4;
	dxtBlock->alpha6 = rgba.color(6).a >> 4;
	dxtBlock->alpha7 = rgba.color(7).a >> 4;
	dxtBlock->alpha8 = rgba.color(8).a >> 4;
	dxtBlock->alpha9 = rgba.color(9).a >> 4;
	dxtBlock->alphaA = rgba.color(10).a >> 4;
	dxtBlock->alphaB = rgba.color(11).a >> 4;
	dxtBlock->alphaC = rgba.color(12).a >> 4;
	dxtBlock->alphaD = rgba.color(13).a >> 4;
	dxtBlock->alphaE = rgba.color(14).a >> 4;
	dxtBlock->alphaF = rgba.color(15).a >> 4;
}

void QuickCompress::compressDXT3(const ColorBlock & rgba, BlockDXT3 * dxtBlock)
{
	compressDXT1(rgba, &dxtBlock->color);
	compressDXT3A(rgba, &dxtBlock->alpha);
}


void QuickCompress::compressDXT5A(const ColorBlock & rgba, AlphaBlockDXT5 * dxtBlock)
{
	uint8 alpha0 = 0;
	uint8 alpha1 = 255;
	
	// Get min/max alpha.
	for (uint i = 0; i < 16; i++)
	{
		uint8 alpha = rgba.color(i).a;
		alpha0 = max(alpha0, alpha);
		alpha1 = min(alpha1, alpha);
	}
	
	AlphaBlockDXT5 block;
	block.alpha0 = alpha0 - (alpha0 - alpha1) / 34;
	block.alpha1 = alpha1 + (alpha0 - alpha1) / 34;
	uint besterror = computeAlphaIndices(rgba, &block);
	
	AlphaBlockDXT5 bestblock = block;
	
	while(true)
	{
		optimizeAlpha8(rgba, &block);
		uint error = computeAlphaIndices(rgba, &block);
		
		if (error >= besterror)
		{
			// No improvement, stop.
			break;
		}
		if (sameIndices(block, bestblock))
		{
			bestblock = block;
			break;
		}
		
		besterror = error;
		bestblock = block;
	};
	
	// Copy best block to result;
	*dxtBlock = bestblock;
}

void QuickCompress::compressDXT5(const ColorBlock & rgba, BlockDXT5 * dxtBlock)
{
	compressDXT1(rgba, &dxtBlock->color);
	compressDXT5A(rgba, &dxtBlock->alpha);
}

